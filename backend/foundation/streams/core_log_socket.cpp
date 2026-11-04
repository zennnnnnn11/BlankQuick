#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <algorithm>
#include <blank/qt/child.hpp>
#include <blank/streams/core_log_socket.hpp>
#include <chrono>

CoreLogSocket::CoreLogSocket(QObject* parent) : QObject(parent)
{
    network_.setProxy(QNetworkProxy::NoProxy);
    retry_.setSingleShot(true);
    watchdog_.setSingleShot(true);
    connect(&retry_, &QTimer::timeout, this, &CoreLogSocket::open);
    connect(&watchdog_, &QTimer::timeout, this, &CoreLogSocket::checkHealth);
}

CoreLogSocket::~CoreLogSocket()
{
    discard();
}

bool CoreLogSocket::connected() const noexcept
{
    return connected_;
}

void CoreLogSocket::discard()
{
    ++generation_;
    if (socket_)
    {
        auto* previous = socket_.data();
        socket_.clear();
        QObject::disconnect(previous, nullptr, this, nullptr);
        previous->abort();
        previous->deleteLater();
    }
    if (health_)
    {
        QObject::disconnect(health_, nullptr, this, nullptr);
        health_->abort();
        health_->deleteLater();
        health_.clear();
    }
}

void CoreLogSocket::stop()
{
    running_ = false;
    connected_ = false;
    retry_.stop();
    watchdog_.stop();
    discard();
    Q_EMIT stateChanged(false, tr("日志流已停止"));
}

void CoreLogSocket::start(const QNetworkRequest& request)
{
    if (running_ && request.url() == request_.url() &&
        request.rawHeader("Authorization") == request_.rawHeader("Authorization"))
    {
        return;
    }
    stop();
    request_ = request;
    running_ = true;
    retryMs_ = 1000;
    open();
}

void CoreLogSocket::open()
{
    if (!running_ || socket_)
    {
        return;
    }
    auto* socket =
        &blank::qt::makeChild<QWebSocket>(*this, QString{}, QWebSocketProtocol::VersionLatest);
    socket_ = socket;
    const auto generation = generation_;
    socket->setProxy(QNetworkProxy::NoProxy);
    socket->setReadBufferSize(32768);
    socket->setMaxAllowedIncomingFrameSize(16384);
    socket->setMaxAllowedIncomingMessageSize(16384);
    connect(socket, &QWebSocket::connected, this,
            [this, generation]
            {
                if (generation != generation_)
                {
                    return;
                }
                connected_ = true;
                retryMs_ = 1000;
                watchdog_.start(std::chrono::milliseconds{10000});
                Q_EMIT stateChanged(true, tr("实时日志已连接"));
            });
    connect(socket, &QWebSocket::textMessageReceived, this,
            [this, generation](const QString& text)
            {
                if (generation != generation_)
                {
                    return;
                }
                const auto object = QJsonDocument::fromJson(text.toUtf8()).object();
                const auto level = object.value(QStringLiteral("type")).toString();
                if (!object.value(QStringLiteral("payload")).isString() ||
                    (level != QStringLiteral("debug") && level != QStringLiteral("info") &&
                     level != QStringLiteral("warning") && level != QStringLiteral("error")))
                {
                    retry(tr("日志消息无效，正在重连"));
                    return;
                }
                Q_EMIT message(text);
            });
    connect(socket, &QWebSocket::binaryMessageReceived, this,
            [this, generation](const QByteArray&)
            {
                if (generation == generation_)
                {
                    retry(tr("日志消息类型无效，正在重连"));
                }
            });
    connect(socket, &QWebSocket::errorOccurred, this,
            [this, generation](QAbstractSocket::SocketError)
            {
                if (generation == generation_)
                {
                    retry(tr("日志连接失败，正在重连"));
                }
            });
    connect(socket, &QWebSocket::disconnected, this,
            [this, generation]
            {
                if (generation == generation_)
                {
                    retry(tr("日志连接断开，正在重连"));
                }
            });
    watchdog_.start(std::chrono::milliseconds{5000});
    socket->open(request_);
}

void CoreLogSocket::retry(const QString& status)
{
    watchdog_.stop();
    discard();
    connected_ = false;
    if (running_)
    {
        retry_.start(retryMs_);
        retryMs_ = std::min(retryMs_ * 2, 10000);
    }
    Q_EMIT stateChanged(false, status);
}

void CoreLogSocket::checkHealth()
{
    if (!connected_)
    {
        retry(tr("日志握手超时，正在重连"));
        return;
    }
    // This core never reads WebSocket Ping frames. Quiet logs are normal; probe HTTP liveness.
    auto request = request_;
    auto url = request.url();
    url.setScheme(QStringLiteral("http"));
    url.setPath(QStringLiteral("/version"));
    url.setQuery(QString{});
    request.setUrl(url);
    request.setTransferTimeout(5000);
    auto* reply = network_.get(request);
    health_ = reply;
    reply->setReadBufferSize(4097);
    connect(reply, &QNetworkReply::readyRead, this,
            [reply]
            {
                if (reply->bytesAvailable() > 4096)
                {
                    reply->abort();
                }
            });
    const auto generation = generation_;
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, generation]
            {
                reply->deleteLater();
                if (generation != generation_)
                {
                    return;
                }
                health_.clear();
                const auto bytes = reply->readAll();
                if (reply->error() != QNetworkReply::NoError ||
                    reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200 ||
                    bytes.size() > 4096 ||
                    QJsonDocument::fromJson(bytes)
                        .object()
                        .value(QStringLiteral("version"))
                        .toString()
                        .isEmpty())
                {
                    retry(tr("日志流内核健康检查失败，正在重连"));
                    return;
                }
                watchdog_.start(std::chrono::milliseconds{10000});
                Q_EMIT stateChanged(true, tr("实时日志已连接"));
            });
}
