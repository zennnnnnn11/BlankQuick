#include <AclAPI.h>
#include <QJsonDocument>
#include <bit>
#include <blank/ipc/service_pipe.hpp>
#include <chrono>
#include <exception>
#include <system_error>
#include <tuple>

namespace {
blank::windows::LocalMemory pipeDescriptor(const QStringList& sessions) {
    auto sddl = QStringLiteral("O:SYG:SYD:P(A;;GA;;;SY)(A;;GA;;;BA)");
    for (const auto& sid : sessions) {
        // Specific rights omit FILE_CREATE_PIPE_INSTANCE, unlike GENERIC_WRITE.
        sddl += QStringLiteral("(A;;0x0012019b;;;%1)").arg(sid);
    }
    return securityDescriptor(sddl);
}
} // namespace

SecurePipe::SecurePipe(QObject* parent) : QObject(parent) {
    peer_.setReadBufferSize(qint64{16} * 1024 * 1024);
    authentication_.setSingleShot(true);
    sessions_.setInterval(std::chrono::milliseconds{2000});
    connect(&authentication_, &QTimer::timeout, this, &SecurePipe::disconnectPeer);
    connect(&sessions_, &QTimer::timeout, this, &SecurePipe::refreshSecurity);
    connect(&peer_, &QLocalSocket::readyRead, this, &SecurePipe::read);
    connect(&peer_, &QLocalSocket::disconnected, this, [this] {
        authentication_.stop();
        ownerEvent_.reset();
        input_.clear();
        Q_EMIT closed();
    });
}

SecurePipe::~SecurePipe() {
    QObject::disconnect(this, nullptr, nullptr, nullptr);
    sessions_.stop();
    authentication_.stop();
    ownerEvent_.reset();
    acceptEvent_.reset();
    peer_.abort();
    if (waiting_ && anchor_) {
        const bool canceled = CancelIoEx(anchor_.get(), &connection_) != FALSE;
        if (canceled && WaitForSingleObject(event_.get(), 3000) == WAIT_TIMEOUT) {
            TerminateProcess(GetCurrentProcess(), ERROR_TIMEOUT);
        }
    }
}

void SecurePipe::listen() {
    allowed_ = loggedOnSids();
    const auto descriptor = pipeDescriptor(allowed_);
    SECURITY_ATTRIBUTES security{
        .nLength = sizeof(SECURITY_ATTRIBUTES),
        .lpSecurityDescriptor = descriptor.get(),
        .bInheritHandle = FALSE,
    };
    anchor_.reset(CreateNamedPipeW(
        servicePipe, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536,
        65536, 0, &security));
    blank::windows::check(anchor_.get() != INVALID_HANDLE_VALUE);
    event_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    blank::windows::check(event_ != nullptr);
    acceptEvent_ = std::make_unique<QWinEventNotifier>(event_.get());
    connect(acceptEvent_.get(), &QWinEventNotifier::activated, this, &SecurePipe::accepted);
    resume();
    sessions_.start();
}

void SecurePipe::resume() try {
    if (waiting_ || peer_.state() != QLocalSocket::UnconnectedState) {
        return;
    }
    identity_ = {};
    DisconnectNamedPipe(anchor_.get());
    connection_ = {};
    connection_.hEvent = event_.get();
    blank::windows::check(ResetEvent(event_.get()) != FALSE);
    waiting_ = true;
    acceptEvent_->setEnabled(true);
    if (ConnectNamedPipe(anchor_.get(), &connection_) != FALSE ||
        GetLastError() == ERROR_PIPE_CONNECTED) {
        QTimer::singleShot(std::chrono::milliseconds{0}, this, &SecurePipe::accepted);
    } else if (GetLastError() != ERROR_IO_PENDING) {
        blank::windows::check(false);
    }
} catch (const std::exception& error) {
    Q_EMIT failed(QString::fromUtf8(error.what()));
}

void SecurePipe::accepted() try {
    if (!waiting_) {
        return;
    }
    waiting_ = false;
    acceptEvent_->setEnabled(false);
    HANDLE raw = nullptr;
    blank::windows::check(DuplicateHandle(GetCurrentProcess(), anchor_.get(), GetCurrentProcess(),
                                          &raw, 0, FALSE, DUPLICATE_SAME_ACCESS) != FALSE);
    blank::windows::Handle duplicate(raw);
    // QLocalSocket owns a duplicate; the anchor remains alive to prevent pipe-name squatting.
    blank::windows::check(peer_.setSocketDescriptor(std::bit_cast<qintptr>(duplicate.get())));
    std::ignore = duplicate.release();
    authentication_.start(std::chrono::milliseconds{5000});
} catch (const std::exception& error) {
    Q_EMIT failed(QString::fromUtf8(error.what()));
    disconnectPeer();
}

void SecurePipe::read() try {
    if (peer_.state() != QLocalSocket::ConnectedState) {
        return;
    }
    if (!input_.append(peer_.readAll())) {
        disconnectPeer();
        return;
    }
    if (identity_.process == 0) {
        identity_ = identifyPipeClient(anchor_.get(), allowed_);
        ownerEvent_ = std::make_unique<QWinEventNotifier>(identity_.lifetime.get());
        connect(ownerEvent_.get(), &QWinEventNotifier::activated, this, [this] {
            ownerEvent_->setEnabled(false);
            disconnectPeer();
        });
        authentication_.stop();
    }
    for (int count = 0; count < 32; ++count) {
        const auto frame = input_.next();
        if (!frame) {
            disconnectPeer();
            return;
        }
        if (!frame->has_value()) {
            return;
        }
        Q_EMIT message(**frame);
        if (peer_.state() != QLocalSocket::ConnectedState) {
            return;
        }
    }
    if (input_.hasFrame()) {
        QTimer::singleShot(std::chrono::milliseconds{0}, this, &SecurePipe::read);
    }
} catch (const std::exception&) {
    disconnectPeer();
}

void SecurePipe::sendEvent(const QJsonObject& frame) {
    // Live diagnostics may be dropped under backpressure; control replies must still get through.
    if (peer_.bytesToWrite() < qint64{1024} * 1024) {
        send(frame);
    }
}

void SecurePipe::send(const QJsonObject& frame) {
    if (peer_.state() != QLocalSocket::ConnectedState) {
        return;
    }
    const auto bytes = blank::protocol::encodeFrame(frame);
    if (!bytes || peer_.bytesToWrite() + bytes->size() > blank::protocol::frameLimit ||
        peer_.write(*bytes) != bytes->size()) {
        disconnectPeer();
    }
}

void SecurePipe::disconnectPeer() {
    peer_.abort();
}

const PipeIdentity& SecurePipe::identity() const noexcept {
    return identity_;
}

void SecurePipe::refreshSecurity() try {
    const auto current = loggedOnSids();
    if (current == allowed_) {
        return;
    }
    const auto descriptor = pipeDescriptor(current);
    BOOL present = FALSE;
    BOOL inherited = FALSE;
    ACL* acl = nullptr;
    blank::windows::check(GetSecurityDescriptorDacl(descriptor.get(), &present, &acl, &inherited) !=
                              FALSE &&
                          (present != 0));
    const auto status =
        SetSecurityInfo(anchor_.get(), SE_KERNEL_OBJECT,
                        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr,
                        nullptr, acl, nullptr);
    if (status != ERROR_SUCCESS) {
        throw std::system_error(static_cast<int>(status), std::system_category());
    }
    allowed_ = current;
    if (identity_.process != 0 && !allowed_.contains(identity_.logonSid)) {
        disconnectPeer();
    }
} catch (const std::exception& error) {
    disconnectPeer();
    Q_EMIT failed(QString::fromUtf8(error.what()));
}
