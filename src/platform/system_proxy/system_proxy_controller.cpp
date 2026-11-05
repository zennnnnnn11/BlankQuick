#include <Windows.h>

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <blank/system_proxy/system_proxy_controller.hpp>
#include <blank/user_proxy/user_proxy.hpp>
#include <chrono>
#include <exception>
#include <utility>

SystemProxyController::SystemProxyController(SystemProxyOptions options, QObject* parent)
    : QObject(parent), options_(std::move(options)) {
    worker_.setCreateProcessArgumentsModifier(
        [](QProcess::CreateProcessArguments* arguments) { arguments->flags |= CREATE_NO_WINDOW; });
    deadline_.setSingleShot(true);
    connect(&worker_, &QProcess::readyReadStandardOutput, this, &SystemProxyController::readOutput);
    connect(&worker_, &QProcess::finished, this, &SystemProxyController::finished);
    connect(&worker_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            failure_ = Failure::Start;
            message_ = tr("无法启动系统代理守护进程：%1").arg(worker_.errorString());
            finished();
        }
    });
    connect(&deadline_, &QTimer::timeout, this, &SystemProxyController::timedOut);
}

SystemProxyController::LeaseState SystemProxyController::leaseState() const noexcept {
    switch (state_) {
    case State::Off:
        return LeaseState::Released;
    case State::Foreign:
        return LeaseState::ForeignOwner;
    case State::On:
        return LeaseState::Owned;
    case State::Recovering:
    case State::Applying:
    case State::Restoring:
        return LeaseState::Pending;
    case State::RecoveryRequired:
        return LeaseState::Uncertain;
    }
    return LeaseState::Uncertain;
}

bool SystemProxyController::active() const noexcept {
    return claim_ == Claim::Owned;
}

SystemProxyController::State SystemProxyController::state() const noexcept {
    return state_;
}

SystemProxyController::Failure SystemProxyController::failure() const noexcept {
    return failure_;
}

bool SystemProxyController::helperRunning() const noexcept {
    return worker_.state() != QProcess::NotRunning;
}

bool SystemProxyController::recoverable() const noexcept {
    return !helperRunning() && !busy() && state_ != State::On;
}

bool SystemProxyController::busy() const noexcept {
    return state_ == State::Recovering || state_ == State::Applying || state_ == State::Restoring;
}

bool SystemProxyController::available() const noexcept {
    return (state_ == State::Off || state_ == State::On) && !context_.busy && context_.ready &&
           context_.processId != 0 && context_.mixedPort != 0;
}

const QString& SystemProxyController::message() const noexcept {
    return message_;
}

void SystemProxyController::launch(const QStringList& arguments, State nextState) {
    state_ = nextState;
    result_.clear();
    output_.clear();
    error_.clear();
    failure_ = Failure::None;
    environmentPending_ = false;
    worker_.start(options_.helper, arguments);
    deadline_.start(options_.operationTimeout);
    Q_EMIT changed();
}

void SystemProxyController::recover() {
    if (worker_.state() != QProcess::NotRunning) {
        return;
    }
    message_ = tr("正在核对上次系统代理恢复记录");
    launch({QStringLiteral("recover"), options_.journal}, State::Recovering);
}

void SystemProxyController::checkEnvironment() {
    if (state_ == State::On && claim_ == Claim::Owned && !environmentPending_ &&
        worker_.state() == QProcess::Running) {
        environmentPending_ = worker_.write("v", 1) == 1;
        if (!environmentPending_) {
            message_ = tr("无法请求系统代理保活核验");
            Q_EMIT changed();
        }
    }
}

void SystemProxyController::setEnabled(bool enabled) {
    if (!enabled) {
        restore();
        return;
    }
    if (!available() || state_ != State::Off) {
        return;
    }
    try {
        const auto owner = static_cast<quint32>(QCoreApplication::applicationPid());
        const auto core = context_.processId;
        const QStringList arguments{
            QStringLiteral("guard"),
            options_.journal,
            QString::number(owner),
            processBirth(owner),
            QString::number(core),
            processBirth(core),
            QString::number(context_.mixedPort),
        };
        claim_ = Claim::Uncertain;
        message_ = tr("正在保存原配置并开启系统代理");
        launch(arguments, State::Applying);
    } catch (const std::exception& error) {
        message_ = tr("无法开启系统代理：%1").arg(QString::fromLocal8Bit(error.what()));
        Q_EMIT changed();
    }
}

void SystemProxyController::restore() {
    // 关闭辅助进程的输入管道是请求恢复，不是强杀。辅助进程必须有机会执行
    // journal 中的回退；进程已结束但租约未确认时，启动显式恢复流程复查现场。
    if (helperRunning()) {
        if (state_ == State::Restoring || state_ == State::RecoveryRequired) {
            return;
        }
        worker_.closeWriteChannel();
        state_ = State::Restoring;
        message_ = tr("正在还原系统代理");
        deadline_.start(options_.restoreTimeout);
        Q_EMIT changed();
    } else if (claim_ != Claim::Released || state_ == State::RecoveryRequired) {
        recover();
    }
}

void SystemProxyController::timedOut() {
    failure_ = Failure::Timeout;
    if (state_ == State::Restoring) {
        state_ = State::RecoveryRequired;
        message_ = tr("系统代理恢复超时，尚未确认释放；守护进程退出后可重试，内核保持运行");
        Q_EMIT changed();
        return;
    }
    restore();
    message_ = tr("系统代理操作超时，正在等待还原；内核保持运行");
    Q_EMIT changed();
}

void SystemProxyController::protocolFailure() {
    output_.clear();
    failure_ = Failure::Protocol;
    error_ = tr("系统代理守护进程响应无效或超限");
    if (helperRunning()) {
        restore();
    }
}

void SystemProxyController::readOutput() {
    output_ += worker_.readAllStandardOutput();
    if (output_.size() > 65536) {
        protocolFailure();
        return;
    }
    qsizetype end = 0;
    while ((end = output_.indexOf('\n')) >= 0) {
        const auto object = QJsonDocument::fromJson(output_.first(end)).object();
        output_.remove(0, end + 1);
        const auto reportedState = object.value(QStringLiteral("state")).toString();
        if (reportedState.isEmpty()) {
            protocolFailure();
            return;
        }
        message_ = object.value(QStringLiteral("message")).toString().left(1000);
        if (reportedState == QStringLiteral("error")) {
            error_ = message_;
            failure_ = Failure::Helper;
        } else if (reportedState == QStringLiteral("checked") ||
                   reportedState == QStringLiteral("foreign") ||
                   reportedState == QStringLiteral("check-error")) {
            environmentPending_ = false;

            Q_EMIT changed();
            continue;
        } else if (reportedState == QStringLiteral("owned")) {
            claim_ = Claim::Owned;
            if (state_ == State::Applying) {
                state_ = State::On;
                deadline_.stop();
            }
        } else if (reportedState != QStringLiteral("restored") &&
                   reportedState != QStringLiteral("blocked") &&
                   reportedState != QStringLiteral("conflict")) {
            protocolFailure();
            return;
        }
        result_ = reportedState;
        Q_EMIT changed();
    }
}

void SystemProxyController::finished() {
    // 退出码或进程结束不能证明代理已恢复，只接受协议中明确的 restored 事实。
    // 其余情况保留恢复要求，防止上层提前停止内核，留下指向失效端口的系统代理。
    environmentPending_ = false;
    deadline_.stop();
    readOutput();
    if (result_ == QStringLiteral("restored")) {
        claim_ = Claim::Released;
        state_ = State::Off;
        if (!error_.isEmpty()) {
            message_ = tr("操作未完成，系统代理已还原：%1").arg(error_);
        }
    } else if (result_ == QStringLiteral("blocked") && claim_ == Claim::Released) {

        state_ = State::Foreign;
    } else {
        state_ = State::RecoveryRequired;
        if (claim_ == Claim::Released) {
            claim_ = Claim::Uncertain;
        }
        if (failure_ == Failure::None) {
            failure_ = result_ == QStringLiteral("conflict") ? Failure::Conflict : Failure::Helper;
        }
        if (message_.isEmpty() || result_ == QStringLiteral("owned")) {
            message_ = tr("尚未确认系统代理还原，保留内核；请重试关闭系统代理");
        }
    }
    Q_EMIT changed();
}

void SystemProxyController::updateContext(SystemProxyContext context) {
    if (context_ == context) {
        return;
    }
    const bool lostKernel = context_.ready && !context.ready;
    context_ = context;
    if (lostKernel && claim_ != Claim::Released && state_ != State::Restoring) {
        restore();
    }
    Q_EMIT changed();
}
