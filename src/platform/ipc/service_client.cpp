#include <QJsonDocument>
#include <algorithm>
#include <bit>
#include <blank/ipc/service_client.hpp>
#include <blank/ipc/service_security.hpp>
#include <blank/qt/child.hpp>
#include <chrono>
#include <cstring>
#include <exception>
#include <tuple>
#include <utility>

PipeReply::PipeReply(QObject* parent) : QNetworkReply(parent) {
    open(QIODevice::ReadOnly | QIODevice::Unbuffered);
    deadline_.setSingleShot(true);
    connect(&deadline_, &QTimer::timeout, this, [this] { complete(503, {}, tr("服务请求超时")); });
    deadline_.start(std::chrono::milliseconds{30000});
}

void PipeReply::setTimeout(int milliseconds) {
    deadline_.start(milliseconds);
}

void PipeReply::abort() {
    // 这里只取消客户端等待，不代表服务端指令已停止执行。上层必须按事务阶段
    // 判断结果是否不确定，不能把 OperationCanceledError 等同于“现场没有变化”。
    if (!isFinished()) {
        setError(OperationCanceledError, tr("服务请求已取消"));
        complete(499, {});
    }
}

qint64 PipeReply::bytesAvailable() const {
    return body_.size() - offset_ + QNetworkReply::bytesAvailable();
}

qint64 PipeReply::readData(char* data, qint64 maximum) {
    const auto count = std::min(maximum, body_.size() - offset_);
    if (count <= 0) {
        return isFinished() ? -1 : 0;
    }
    std::memcpy(data, QByteArrayView(body_).sliced(offset_).data(),
                static_cast<std::size_t>(count));
    offset_ += count;
    return count;
}

void PipeReply::complete(int status, QByteArray body, const QString& failure) {
    if (isFinished()) {
        return;
    }
    deadline_.stop();
    body_ = std::move(body);
    setAttribute(QNetworkRequest::HttpStatusCodeAttribute, status);
    if (!failure.isEmpty() || (status >= 400 && error() == NoError)) {
        setError(UnknownNetworkError, failure.isEmpty() ? tr("服务拒绝请求") : failure);
    }
    setFinished(true);
    const QPointer<PipeReply> lifetime{this};
    Q_EMIT readyRead();
    // A readyRead slot may destroy this reply; QPointer observes that deletion.
    // cppcheck-suppress knownConditionTrueFalse
    if (lifetime) {
        Q_EMIT finished();
    }
}

ServiceClient::ServiceClient(QObject* parent) : QObject(parent) {
    socket_.setReadBufferSize(qint64{16} * 1024 * 1024);
    connect(&socket_, &QLocalSocket::connected, this, &ServiceClient::connected);
    connect(&socket_, &QLocalSocket::readyRead, this, &ServiceClient::received);
    connect(&socket_, &QLocalSocket::disconnected, this, [this] { fail(tr("服务连接已断开")); });
    connect(&socket_, &QLocalSocket::errorOccurred, this,
            [this](QLocalSocket::LocalSocketError) { fail(socket_.errorString()); });
}

ServiceClient::~ServiceClient() {
    QObject::disconnect(&socket_, nullptr, this, nullptr);
    QObject::disconnect(this, nullptr, nullptr, nullptr);
    socket_.abort();
}

PipeReply* ServiceClient::request(const QString& operation, const QJsonObject& arguments) {
    // 先创建回复对象并排队，连接通过服务端身份核验后才发送积压帧。
    // 本地拒绝也延后一轮完成，让调用者有机会先连接 finished 信号。
    auto* reply = &blank::qt::makeChild<PipeReply>(*this, nullptr);
    if (pending_.size() >= 16) {
        QTimer::singleShot(std::chrono::milliseconds{0}, reply,
                           [reply] { reply->complete(429, {}, QStringLiteral("服务请求过多")); });
        return reply;
    }
    const auto serial = ++nextId_;
    const auto id = QString::number(serial);
    const auto encoded = blank::protocol::encodeFrame(QJsonObject{
        {QStringLiteral("id"), id},
        {QStringLiteral("op"), operation},
        {QStringLiteral("args"), arguments},
    });
    if (!encoded) {
        QTimer::singleShot(std::chrono::milliseconds{0}, reply, [reply] {
            reply->complete(413, {}, QStringLiteral("服务请求超过限制"));
        });
        return reply;
    }
    const auto& frame = *encoded;
    pending_.insert(id, reply);
    connect(reply, &QNetworkReply::finished, this, [this, id, serial] {
        pending_.remove(id);
        unsent_.remove(serial);
    });
    if (verified_) {
        if (socket_.write(frame) != frame.size()) {
            QTimer::singleShot(std::chrono::milliseconds{0}, this,
                               [this] { fail(tr("服务请求发送失败")); });
        }
    } else {
        unsent_.insert(serial, frame);
        if (socket_.state() == QLocalSocket::UnconnectedState) {
            QTimer::singleShot(std::chrono::milliseconds{0}, this, &ServiceClient::connectPipe);
        }
    }
    return reply;
}

void ServiceClient::connectPipe() try {
    if (verified_ || pending_.isEmpty()) {
        return;
    }
    constexpr DWORD access = FILE_READ_DATA | FILE_WRITE_DATA | FILE_READ_EA | FILE_WRITE_EA |
                             FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | READ_CONTROL |
                             SYNCHRONIZE;
    blank::windows::Handle pipe(CreateFileW(
        servicePipe, access, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
    if (pipe.get() == INVALID_HANDLE_VALUE && GetLastError() == ERROR_PIPE_BUSY) {

        QTimer::singleShot(std::chrono::milliseconds{100}, this, &ServiceClient::connectPipe);
        return;
    }
    blank::windows::check(pipe.get() != INVALID_HANDLE_VALUE);
    verifyServicePipe(pipe.get());

    blank::windows::check(socket_.setSocketDescriptor(std::bit_cast<qintptr>(pipe.get())));
    std::ignore = pipe.release();
    connected();
} catch (const std::exception& error) {
    fail(QString::fromUtf8(error.what()));
}

void ServiceClient::connected() try {

    verifyServicePipe(std::bit_cast<HANDLE>(socket_.socketDescriptor()));
    verified_ = true;
    const auto frames = std::exchange(unsent_, {});
    if (std::ranges::any_of(
            frames, [this](const auto& frame) { return socket_.write(frame) != frame.size(); })) {
        fail(tr("服务请求发送失败"));
    }
} catch (const std::exception& error) {
    fail(QString::fromUtf8(error.what()));
}

void ServiceClient::received() {

    if (!verified_ || socket_.state() != QLocalSocket::ConnectedState) {
        return;
    }
    if (!input_.append(socket_.readAll())) {
        fail(tr("服务消息超过限制"));
        return;
    }
    for (int count = 0; count < 32; ++count) {
        const auto frame = input_.next();
        if (!frame) {
            fail(tr("服务消息格式无效"));
            return;
        }
        if (!frame->has_value()) {
            return;
        }
        const auto& object = **frame;
        if (object.value(QStringLiteral("event")).toString() == QStringLiteral("logs")) {
            Q_EMIT logs(object.value(QStringLiteral("value")).toString().left(16384));
            continue;
        }
        if (object.value(QStringLiteral("event")).toString() == QStringLiteral("log-state")) {
            Q_EMIT logState(object.value(QStringLiteral("connected")).toBool(),
                            object.value(QStringLiteral("message")).toString().left(1024));
            continue;
        }
        if (object.value(QStringLiteral("event")).toString() == QStringLiteral("environment")) {
            Q_EMIT environment(object.value(QStringLiteral("message")).toString().left(1024),
                               object.value(QStringLiteral("repairing")).toBool());
            continue;
        }
        if (object.value(QStringLiteral("event")).toString() == QStringLiteral("traffic")) {
            Q_EMIT traffic(object.value(QStringLiteral("value")).toString());
            continue;
        }
        if (object.value(QStringLiteral("event")).toString() == QStringLiteral("stopped")) {
            Q_EMIT coreStopped(object.value(QStringLiteral("error")).toString());
            continue;
        }
        const auto id = object.value(QStringLiteral("id")).toString();
        if (auto const reply = pending_.value(id)) {
            reply->complete(
                object.value(QStringLiteral("status")).toInt(500),
                QByteArray::fromBase64(object.value(QStringLiteral("body")).toString().toLatin1()),
                object.value(QStringLiteral("error")).toString());
        }
    }
    if (input_.hasFrame()) {
        QTimer::singleShot(std::chrono::milliseconds{0}, this, &ServiceClient::received);
    }
}

void ServiceClient::fail(const QString& message) {
    // 先移出所有 pending，再逐个发布失败。complete 会同步触发 finished 槽，
    // 这些槽可能删除回复或修改请求表，不能在遍历原容器时允许它们改动容器。
    const bool wasConnected = std::exchange(verified_, false);
    input_.clear();
    unsent_.clear();
    const auto replies = std::exchange(pending_, {});
    socket_.abort();
    for (const auto& reply : replies) {
        if (reply) {
            reply->complete(503, {}, message);
        }
    }
    if (wasConnected) {
        Q_EMIT lost(message);
    }
}
