#include <QtConcurrentRun>

#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QPointer>
#include <QStandardPaths>
#include <QUrl>
#include <array>
#include <blank/ipc/service_client.hpp>
#include <blank/protocol/service_api.hpp>
#include <blank/runtime/runtime_session.hpp>
#include <blank/runtime_config/runtime_context.hpp>
#include <blank/shared/timeouts.hpp>
#include <chrono>
#include <exception>
#include <utility>

namespace {
constexpr qint64 responseLimit = qint64{8} * 1024 * 1024;

[[nodiscard]] constexpr bool
proxyAllowsTunChange(SystemProxyController::LeaseState lease) noexcept {

    switch (lease) {
    case SystemProxyController::LeaseState::Released:
    case SystemProxyController::LeaseState::ForeignOwner:
    case SystemProxyController::LeaseState::Owned:
        return true;
    case SystemProxyController::LeaseState::Pending:
    case SystemProxyController::LeaseState::Uncertain:
        return false;
    }
    return false;
}

static_assert(proxyAllowsTunChange(SystemProxyController::LeaseState::Released));
static_assert(proxyAllowsTunChange(SystemProxyController::LeaseState::ForeignOwner));
static_assert(proxyAllowsTunChange(SystemProxyController::LeaseState::Owned));
static_assert(!proxyAllowsTunChange(SystemProxyController::LeaseState::Pending));
static_assert(!proxyAllowsTunChange(SystemProxyController::LeaseState::Uncertain));

runtime_config::routing_mode routingMode(const QString& name) noexcept {
    if (name == QStringLiteral("global")) {
        return runtime_config::routing_mode::global;
    }
    return name == QStringLiteral("direct") ? runtime_config::routing_mode::direct
                                            : runtime_config::routing_mode::rule;
}

constexpr TransactionOutcome tunOutcome(bool success, bool activation) noexcept {
    if (success) {
        return TransactionOutcome::Applied;
    }
    return activation ? TransactionOutcome::PartiallyApplied : TransactionOutcome::Uncertain;
}

QJsonObject selections(const QByteArray& bytes) {
    QJsonObject result;
    const auto entries =
        QJsonDocument::fromJson(bytes).object().value(QStringLiteral("proxies")).toObject();
    for (auto item = entries.begin(); item != entries.end(); ++item) {
        const auto value = item.value().toObject();
        if (value.value(QStringLiteral("now")).isString()) {
            result.insert(item.key(), value.value(QStringLiteral("now")));
        }
    }
    return result;
}
} // namespace

RuntimeSession::RuntimeSession(KernelOptions options, SystemProxyOptions proxyOptions,
                               EnvironmentOptions environment, QObject* parent)
    : QObject(parent), options_(std::move(options)), kernel_(options_), switcher_(kernel_),
      environment_(std::move(environment)) {
    // 所有适配器间的协作在这里接线：流量读取器只报告订阅意图，真正的服务命令
    // 由会话发送。回包携带流量代次，防止旧连接的成功结果复活已经关闭的新订阅。
    network_.setProxy(QNetworkProxy::NoProxy);
    healRetry_.setSingleShot(true);
    connect(&healRetry_, &QTimer::timeout, this, &RuntimeSession::heal);
    connect(&checker_, &QFutureWatcher<ActivationCheck>::finished, this,
            &RuntimeSession::checkFinished);
    connect(&switcher_, &ConfigSwitcher::finished, this, &RuntimeSession::switched);
    connect(&switcher_, &ConfigSwitcher::progressChanged, this, &RuntimeSession::operationChanged,
            Qt::QueuedConnection);
    connect(&kernel_, &KernelController::statusChanged, this, &RuntimeSession::kernelChanged);
    connect(&kernel_, &KernelController::stateChanged, this, &RuntimeSession::refreshStreams);
    connect(&kernel_, &KernelController::serviceTraffic, &traffic_,
            &TrafficController::receiveServiceMessage);
    connect(&traffic_, &TrafficController::serviceSubscriptionRequested, this,
            [this](bool enabled, quint64 generation) {
                auto* reply = kernel_.serviceRequest(enabled ? QStringLiteral("traffic")
                                                             : QStringLiteral("traffic-stop"));
                connect(reply, &QNetworkReply::finished, this, [this, reply, generation] {
                    reply->deleteLater();
                    traffic_.serviceRequestFinished(generation,
                                                    reply->error() == QNetworkReply::NoError);
                });
            });
    connect(&logs_, &CoreLogSocket::message, this, &RuntimeSession::logMessage);
    connect(&kernel_, &KernelController::serviceLogs, this, [this](const QString& message) {
        if (logsEnabled_) {
            Q_EMIT logMessage(message);
        }
    });
    connect(&kernel_, &KernelController::serviceEnvironment, this,
            &RuntimeSession::environmentStatus);
    connect(&environment_, &EnvironmentMonitor::failed, this, &RuntimeSession::environmentStatus);
    connect(&environment_, &EnvironmentMonitor::observed, this,
            [this](const NetworkSnapshot& snapshot, EnvironmentBatch batch, bool changed) {
                online_ = snapshot.online;
                const bool resumed = batch.resumed() || (suspended_ && !batch.suspended);
                suspended_ = batch.suspended;
                if (suspended_) {
                    traffic_.environmentChanged(true);
                    logs_.stop();
                    Q_EMIT operationChanged();
                    return;
                }
                const QPointer<RuntimeSession> lifetime{this};
                if (changed || resumed) {
                    traffic_.environmentChanged(false);
                    // cppcheck-suppress knownConditionTrueFalse
                    if (!lifetime) {
                        return;
                    }
                    refreshStreams();
                    // cppcheck-suppress knownConditionTrueFalse
                    if (!lifetime) {
                        return;
                    }
                    if (proxy_) {
                        proxy_->checkEnvironment();
                        // cppcheck-suppress knownConditionTrueFalse
                        if (!lifetime) {
                            return;
                        }
                    }
                    healAttempts_ = 0;
                    healPending_ = !kernel_.serviceManaged();
                }
                heal();
                // cppcheck-suppress knownConditionTrueFalse
                if (lifetime) {
                    Q_EMIT operationChanged();
                }
            });
    if (!proxyOptions.helper.isEmpty() && !proxyOptions.journal.isEmpty()) {
        proxy_ = std::make_unique<SystemProxyController>(std::move(proxyOptions));
        connect(proxy_.get(), &SystemProxyController::changed, this, [this] {
            if (!proxy_) {
                return;
            }
            const QPointer<RuntimeSession> lifetime{this};
            finishStop();
            // cppcheck-suppress knownConditionTrueFalse
            if (lifetime) {
                Q_EMIT operationChanged();
            }
        });
        connect(this, &RuntimeSession::operationChanged, this, &RuntimeSession::refreshProxy,
                Qt::QueuedConnection);
    }
}

RuntimeSession::~RuntimeSession() {
    // 析构只能撤销本对象的观察和回调，不能等待事件循环完成代理恢复。
    // 正常关闭必须先走 shutdown 屏障；这里仍要中断请求，防止回调访问已销毁成员。
    static_cast<void>(cancellation_.request_stop());
    QObject::disconnect(&checker_, nullptr, this, nullptr);
    QObject::disconnect(&kernel_, nullptr, this, nullptr);
    QObject::disconnect(&switcher_, nullptr, this, nullptr);
    if (proxy_) {
        QObject::disconnect(proxy_.get(), nullptr, this, nullptr);
    }
    if (reply_) {
        QObject::disconnect(reply_, nullptr, this, nullptr);
        reply_->abort();
    }
}

KernelController::State RuntimeSession::state() const noexcept {
    return kernel_.state();
}

quint32 RuntimeSession::processId() const noexcept {
    return kernel_.processId();
}

quint16 RuntimeSession::controllerPort() const noexcept {
    return kernel_.port();
}

QString RuntimeSession::dataDirectory() const {
    if (kernel_.state() != KernelController::State::Ready) {
        return {};
    }
    return kernel_.serviceManaged() ? options_.workRoot : kernel_.homeDirectory();
}

QString RuntimeSession::statusText() const {
    return kernel_.statusText();
}

const QString& RuntimeSession::version() const noexcept {
    return kernel_.version();
}

const TrafficController& RuntimeSession::traffic() const noexcept {
    return traffic_;
}

const SystemProxyController* RuntimeSession::systemProxy() const noexcept {
    return proxy_.get();
}

bool RuntimeSession::busy() const noexcept {
    return stopping() || uncertain_ || kernel_.maintenance() || transaction_.has_value() ||
           switcher_.busy() || reply_ || (proxy_ && proxy_->busy());
}

bool RuntimeSession::uncertain() const noexcept {
    return uncertain_;
}

quint16 RuntimeSession::mixedPort() const noexcept {
    return mixedPort_;
}

bool RuntimeSession::tunActive() const noexcept {
    return tunActive_;
}

RuntimeStatus RuntimeSession::status() const noexcept {
    // 先收集事实，再推导操作能力。这里不触发 I/O，也不推进任何状态机，
    // 因此界面反复读取状态不会产生新的系统操作。
    RuntimeStatus result;
    result.kernel = kernel_.state();
    result.resources = kernel_.resources();
    result.kernelEpoch = generation_;
    result.runtimeRevision = runtimeRevision_;
    result.active = activeIdentity_;
    result.pending = transaction_ && transaction_->result.kind == TransactionKind::Activation
                         ? transaction_->result.identity
                         : nullptr;
    result.lifecycle = lifecycle_;
    result.stopBarrier = stopBarrier();
    result.routing = routing_;
    result.maintenance = kernel_.maintenance();
    result.configLocked = configLocked();
    result.tunActive = tunActive_;
    result.stopping = stopping();
    result.shuttingDown = closing();
    result.proxyBusy = proxy_ && proxy_->busy();
    result.suspended = suspended_;
    result.online = online_;
    result.serviceManaged = kernel_.serviceManaged();
    result.processId = processId();
    result.controllerPort = controllerPort();
    result.mixedPort = mixedPort_;
    result.certainty = active_.isEmpty() ? ConfigurationCertainty::NoConfiguration
                                         : ConfigurationCertainty::Confirmed;
    if (uncertain_) {
        result.certainty = ConfigurationCertainty::Uncertain;
    }
    if (proxy_) {
        result.proxyLease = proxy_->leaseState();
        result.proxyState = proxy_->state();
        result.proxyFailure = proxy_->failure();
    }
    if (transaction_) {
        result.requestId = transaction_->result.requestId;
        result.stage = transaction_->result.stage;
    }
    result.operation = operation();
    if (preparing()) {
        result.stage = TransactionStage::Preparation;
    }
    if (switcher_.busy()) {
        result.stage = switcher_.phase();
    }
    result.rollingBack = switcher_.busy() && switcher_.rollingBack();
    const bool mutableNow = canMutate();
    result.capabilities = {
        .activate = activationRejection() == ActivationRejection::None,
        .changeMode = mutableNow && !active_.isEmpty(),
        .selectProxy = mutableNow && !active_.isEmpty(),
        .setTun = canSetTun(),
        .request = mutableNow,
        .enableSystemProxy = proxy_ && mutableNow && !closing() && proxy_->available(),
        .disableSystemProxy = static_cast<bool>(proxy_),
        .recoverSystemProxy = proxy_ && proxy_->recoverable() && (stopping() || !busy()),
    };
    return result;
}

RuntimeOperation RuntimeSession::operation() const noexcept {
    if (stopping()) {
        return RuntimeOperation::Stopping;
    }
    if (preparing()) {
        return RuntimeOperation::Preparing;
    }
    if (switcher_.busy()) {
        return RuntimeOperation::Switching;
    }
    if (transaction_ && transaction_->execution == Execution::ActivationTun) {
        return RuntimeOperation::Tun;
    }
    if (transaction_) {
        switch (transaction_->result.kind) {
        case TransactionKind::Activation:
        case TransactionKind::Tun:
            return RuntimeOperation::Tun;
        case TransactionKind::Mode:
            return RuntimeOperation::Mode;
        case TransactionKind::Selection:
            return RuntimeOperation::Selection;
        }
    }
    if (reply_) {
        return healing_ ? RuntimeOperation::Healing : RuntimeOperation::Request;
    }
    return proxy_ && proxy_->busy() ? RuntimeOperation::SystemProxy : RuntimeOperation::Idle;
}

[[maybe_unused]] auto RuntimeSession::lastTransaction() const noexcept
    -> std::shared_ptr<const TransactionResult> {
    return lastTransaction_;
}

bool RuntimeSession::preparing() const noexcept {
    return transaction_ && transaction_->execution == Execution::Preparing;
}

bool RuntimeSession::stopping() const noexcept {
    return lifecycle_ == RuntimeLifecycle::Stopping || lifecycle_ == RuntimeLifecycle::ShuttingDown;
}

bool RuntimeSession::closing() const noexcept {
    return lifecycle_ == RuntimeLifecycle::ShuttingDown || lifecycle_ == RuntimeLifecycle::Closed;
}

StopBarrier RuntimeSession::stopBarrier() const noexcept {
    if (!stopping()) {
        return StopBarrier::None;
    }
    if (preparing()) {
        return StopBarrier::Preparation;
    }
    if (configLocked()) {
        return proxy_ && proxy_->state() == SystemProxyController::State::RecoveryRequired
                   ? StopBarrier::ProxyRecovery
                   : StopBarrier::ProxyRestore;
    }
    return kernel_.state() == KernelController::State::Failed &&
                   kernel_.resources() != KernelController::Resources::Released
               ? StopBarrier::KernelRecovery
               : StopBarrier::KernelCleanup;
}

RuntimeSession::TransactionToken RuntimeSession::transactionToken() const noexcept {
    return {.epoch = generation_, .requestId = transaction_ ? transaction_->result.requestId : 0};
}

bool RuntimeSession::currentTransaction(TransactionToken token) const noexcept {

    return transaction_ && token.epoch == generation_ &&
           token.requestId == transaction_->result.requestId;
}

ActivationRejection RuntimeSession::activationRejection() const noexcept {
    // 拒绝顺序也是对外诊断优先级。特别是租约未释放或 TUN 正在工作时，
    // 不允许替换其依赖的配置；不能只检查内核是否 Ready。
    if (closing() || stopping()) {
        return ActivationRejection::ShuttingDown;
    }
    if (kernel_.state() != KernelController::State::Ready) {
        return ActivationRejection::NotReady;
    }
    if (uncertain_) {
        return ActivationRejection::Uncertain;
    }
    if (busy()) {
        return ActivationRejection::Busy;
    }

    if (configLocked()) {
        return ActivationRejection::ProxyLease;
    }
    return tunActive_ ? ActivationRejection::TunActive : ActivationRejection::None;
}

TransactionResult& RuntimeSession::beginTransaction(TransactionKind kind) {
    auto& pending = transaction_.emplace(PendingTransaction{
        .result =
            {
                .requestId = ++nextRequestId_,
                .kernelEpoch = generation_,
                .kind = kind,
                .identity = activeIdentity_,
            },
    });
    pending.execution =
        kind == TransactionKind::Activation ? Execution::Preparing : Execution::Changing;
    if (kind == TransactionKind::Tun) {
        pending.execution = Execution::Tun;
    }
    return pending.result;
}

void RuntimeSession::finishTransaction(TransactionOutcome outcome, TransactionStage stage,
                                       TransactionError error, const QString& message) {
    if (!transaction_) {
        return;
    }
    transaction_->result.outcome = outcome;
    transaction_->result.stage = stage;
    transaction_->result.error = error;
    transaction_->result.message = message;
    const auto result = std::make_shared<const TransactionResult>(std::move(transaction_->result));
    // 先清空进行中的事务再发信号。Qt 信号可能同步调用界面代码，界面此时读取
    // status 或提交下一个操作，必须看到已经结束的状态；result 独立持有发布内容。
    transaction_.reset();
    lastTransaction_ = result;
    const QPointer<RuntimeSession> lifetime{this};
    Q_EMIT operationChanged();
    // cppcheck-suppress knownConditionTrueFalse
    if (!lifetime) {
        return;
    }
    Q_EMIT transactionFinished(*result);
    // cppcheck-suppress knownConditionTrueFalse
    if (!lifetime) {
        return;
    }
    if (result->kind != TransactionKind::Tun) {
        Q_EMIT activationFinished(outcome == TransactionOutcome::Applied,
                                  outcome == TransactionOutcome::Rejected ||
                                      outcome == TransactionOutcome::RolledBack ||
                                      outcome == TransactionOutcome::Cancelled,
                                  message);
    }
}

void RuntimeSession::cancelTransaction() {
    if (!transaction_ || preparing() || switcher_.busy()) {
        return;
    }
    const bool partial = transaction_->execution == Execution::ActivationTun;
    uncertain_ = true;
    finishTransaction(
        partial ? TransactionOutcome::PartiallyApplied : TransactionOutcome::Uncertain,
        transaction_->result.stage, TransactionError::Cancelled,
        QStringLiteral("Submitted runtime transaction canceled with its kernel session."));
}

bool RuntimeSession::configLocked() const noexcept {
    if (!proxy_) {
        return false;
    }
    const auto lease = proxy_->leaseState();
    return lease != SystemProxyController::LeaseState::Released &&
           lease != SystemProxyController::LeaseState::ForeignOwner;
}

void RuntimeSession::refreshProxy() {
    if (proxy_) {
        proxy_->updateContext({
            .processId = kernel_.processId(),
            .mixedPort = mixedPort_,
            .ready = kernel_.state() == KernelController::State::Ready,
            .busy = busy() || closing(),
        });
    }
}

void RuntimeSession::setTrafficEnabled(bool enabled) {
    if (!enabled || (!closing() && !stopping())) {
        traffic_.setEnabled(enabled);
    }
}

bool RuntimeSession::setSystemProxyEnabled(bool enabled) {
    if (!proxy_) {
        return false;
    }
    refreshProxy();
    if (enabled && (!canMutate() || closing() || !proxy_->available())) {
        return false;
    }
    proxy_->setEnabled(enabled);
    return true;
}

bool RuntimeSession::recoverSystemProxy() {
    if (!proxy_ || !proxy_->recoverable() || (!stopping() && busy())) {
        return false;
    }
    proxy_->recover();
    return true;
}

runtime_config::runtime_context RuntimeSession::context() const {
    if (kernel_.state() != KernelController::State::Ready) {
        return {};
    }
    if (kernel_.serviceManaged()) {
        return runtime_config::make_runtime_context(options_.workRoot.toStdWString());
    }
    return {
        .data_directory = kernel_.homeDirectory().toStdWString(),
        .controller = kernel_.controllerAddress().toStdString(),
        .secret = kernel_.sessionSecret().toStdString(),
    };
}

void RuntimeSession::start() {
    if (closing() || stopping()) {
        return;
    }
    const QPointer<RuntimeSession> lifetime{this};
    environment_.start();
    // cppcheck-suppress knownConditionTrueFalse
    if (!lifetime) {
        return;
    }
    if (proxy_) {
        proxy_->recover();
        // cppcheck-suppress knownConditionTrueFalse
        if (!lifetime) {
            return;
        }
    }
    if (!closing() && !stopping()) {
        kernel_.start();
    }
}

void RuntimeSession::stop() {
    if (lifecycle_ == RuntimeLifecycle::Closed) {
        return;
    }
    const QPointer<RuntimeSession> lifetime{this};
    // 先作废旧代次，再取消各执行步骤。abort/cancel 可能同步发出完成信号，
    // 此时旧回调已经失去推进新状态的资格。
    ++generation_;
    lifecycle_ = closing() ? RuntimeLifecycle::ShuttingDown : RuntimeLifecycle::Stopping;
    healRetry_.stop();
    healing_ = false;
    healPending_ = false;
    static_cast<void>(cancellation_.request_stop());
    if (auto* const reply = reply_.data()) {
        reply->abort();
        // cppcheck-suppress knownConditionTrueFalse
        if (!lifetime) {
            return;
        }
        if (reply_ == reply) {
            reply_.clear();
        }
    }
    switcher_.cancel();
    // cppcheck-suppress knownConditionTrueFalse
    if (!lifetime) {
        return;
    }
    cancelTransaction();
    // cppcheck-suppress knownConditionTrueFalse
    if (!lifetime) {
        return;
    }
    if (proxy_) {
        proxy_->setEnabled(false);
        // cppcheck-suppress knownConditionTrueFalse
        if (!lifetime) {
            return;
        }
    }
    finishStop();
}

void RuntimeSession::shutdown() {
    if (closing()) {
        return;
    }
    lifecycle_ = RuntimeLifecycle::ShuttingDown;
    stop();
}

void RuntimeSession::finishStop() {
    // 代理租约可能还指向当前内核的端口。先等待代理恢复，再停止内核；
    // 无法确认租约时保留屏障，交给显式恢复流程处理。
    if (!stopping() || preparing() || configLocked()) {
        return;
    }
    const bool stopped = kernel_.state() == KernelController::State::Stopped ||
                         (kernel_.state() == KernelController::State::Failed &&
                          kernel_.resources() == KernelController::Resources::Released);
    if (!stopped) {
        kernel_.stop();
        return;
    }
    const QPointer<RuntimeSession> lifetime{this};
    const bool closingRequested = closing();
    lifecycle_ = closingRequested ? RuntimeLifecycle::Closed : RuntimeLifecycle::Open;
    Q_EMIT operationChanged();
    // cppcheck-suppress knownConditionTrueFalse
    if (lifetime && closingRequested) {
        Q_EMIT shutdownFinished();
    }
}

bool RuntimeSession::canMutate() const noexcept {
    return kernel_.state() == KernelController::State::Ready && !busy() && !stopping() &&
           !uncertain_ && !kernel_.maintenance();
}

bool RuntimeSession::canSetTun() const noexcept {
    return canMutate() && kernel_.serviceManaged() && !active_.isEmpty() &&
           (!proxy_ || proxyAllowsTunChange(proxy_->leaseState()));
}

bool RuntimeSession::activate(ActivationInput input) {
    return submitActivation(std::move(input)).accepted();
}

ActivationSubmission RuntimeSession::submitActivation(ActivationInput input) {
    // 接纳检查在会话线程完成；耗时的组装、脚本和 -t 检查交给工作任务。
    // 工作任务得到输入、上下文和取消令牌，不借用本对象中的可变业务状态。
    const auto rejection = activationRejection();
    if (rejection != ActivationRejection::None) {
        return {.rejection = rejection};
    }
    auto& transaction = beginTransaction(TransactionKind::Activation);
    const auto requestId = transaction.requestId;
    const QPointer<RuntimeSession> lifetime{this};
    try {
        transaction.identity = std::make_shared<const ActivationIdentity>(
            ActivationIdentity{.requestId = requestId, .composition = std::move(input.identity)});
        transaction.stage = TransactionStage::Generation;
        cancellation_ = std::stop_source{};
        checkGeneration_ = generation_;
        auto options = options_;
        options.serviceManaged = kernel_.serviceManaged();
        checker_.setFuture(QtConcurrent::run(prepareActivation, std::move(input), context(),
                                             options, cancellation_.get_token()));
    } catch (const std::exception&) {
        finishTransaction(
            TransactionOutcome::Rejected, TransactionStage::Generation,
            TransactionError::Unexpected,
            QStringLiteral("Invalid activation context or unavailable worker resources."));
        // cppcheck-suppress knownConditionTrueFalse
        if (!lifetime) {
            return {.requestId = requestId};
        }
    }
    Q_EMIT operationChanged();
    return {.requestId = requestId};
}

void RuntimeSession::checkFinished() {
    auto result = checker_.future().takeResult();
    if (!transaction_) {
        finishStop();
        return;
    }
    transaction_->result.diagnostics =
        std::make_shared<const ActivationDiagnostics>(std::move(result.diagnostics));
    transaction_->enableTun = result.enableTun;
    const QPointer<RuntimeSession> lifetime{this};
    // 准备成功也不能直接提交：准备期间可能已经停止、换代或失去 Ready 状态。
    // 重新验证会话身份，把“算出了合法配置”和“仍然有权应用”分成两道门。
    if (checkGeneration_ != generation_ || stopping() ||
        kernel_.state() != KernelController::State::Ready ||
        result.code == TransactionError::Cancelled) {
        finishTransaction(TransactionOutcome::Cancelled, result.stage, TransactionError::Cancelled,
                          QStringLiteral("Activation canceled with its kernel session."));
    } else if (!result.error.isEmpty()) {
        finishTransaction(TransactionOutcome::Rejected, result.stage, result.code, result.error);
    } else {
        transaction_->payload = result.payload;
        transaction_->execution = Execution::Switching;
        switcher_.start(QJsonDocument::fromJson(transaction_->payload.toUtf8()).object(), active_,
                        selected_, selected_);
    }
    // cppcheck-suppress knownConditionTrueFalse
    if (!lifetime) {
        return;
    }
    Q_EMIT operationChanged();
    // cppcheck-suppress knownConditionTrueFalse
    if (!lifetime) {
        return;
    }
    finishStop();
}

void RuntimeSession::switched(const SwitchResult& result) {
    if (!transaction_) {
        return;
    }
    uncertain_ = result.outcome == TransactionOutcome::Uncertain;
    transaction_->result.switches = result.diagnostics;
    // 只有配置回读确认后才发布 activeIdentity 和运行修订号。若后续 TUN 失败，
    // 这份配置仍然已经生效，结果必须报告部分应用，不能把身份退回旧版本。
    if (result.outcome == TransactionOutcome::Applied) {
        active_ = std::move(transaction_->payload);
        activeIdentity_ = transaction_->result.identity;
        mixedPort_ =
            static_cast<quint16>(result.general.value(QStringLiteral("mixed-port")).toInteger());
        const auto mode = result.general.value(QStringLiteral("mode")).toString();
        routing_ = routingMode(mode);
        ++runtimeRevision_;
    }
    if (result.outcome == TransactionOutcome::Applied ||
        result.outcome == TransactionOutcome::RolledBack) {
        selected_ = selections(result.proxies);
    }
    if (result.outcome == TransactionOutcome::Applied && transaction_->enableTun) {

        transaction_->execution = Execution::ActivationTun;
        issueTun(true);
        return;
    }
    const auto failure =
        result.diagnostics.empty() ? SwitchDiagnostic{} : result.diagnostics.back();
    finishTransaction(result.outcome,
                      result.diagnostics.empty() ? TransactionStage::FinalProxies : failure.stage,
                      failure.error, result.message);
}

void RuntimeSession::kernelChanged() {
    const QPointer<RuntimeSession> lifetime{this};
    if (kernel_.state() != KernelController::State::Ready) {
        ++generation_;
        healRetry_.stop();
        healing_ = false;
        healPending_ = false;
        static_cast<void>(cancellation_.request_stop());
        if (auto* const reply = reply_.data()) {
            reply->abort();
            // cppcheck-suppress knownConditionTrueFalse
            if (!lifetime) {
                return;
            }
            if (reply_ == reply) {
                reply_.clear();
            }
        }
        cancelTransaction();
        // cppcheck-suppress knownConditionTrueFalse
        if (!lifetime) {
            return;
        }
        mixedPort_ = 0;
        logs_.stop();
    }
    if (kernel_.state() == KernelController::State::Stopped ||
        (kernel_.state() == KernelController::State::Failed &&
         kernel_.resources() == KernelController::Resources::Released)) {
        tunActive_ = false;
        uncertain_ = false;
        active_.clear();
        activeIdentity_.reset();
        routing_.reset();
        selected_ = {};
        finishStop();
        // cppcheck-suppress knownConditionTrueFalse
        if (!lifetime) {
            return;
        }
    }
    Q_EMIT operationChanged();
}

bool RuntimeSession::request(const QByteArray& path, const QByteArray& method,
                             const QByteArray& body) {

    // 这是有限的资源查询入口，不是任意内核 API 的透传口。配置、模式、选择和
    // TUN 写入必须走各自的事务路径，保留准入、回读和不确定结果处理。
    const bool read = method == "GET" && body.isEmpty() &&
                      (path == "/version" || path == "/proxies" || path == "/rules" ||
                       path == "/connections" || path == "/configs");
    const auto resource = serviceResourceEndpoint(path, method, body);
    const auto delay = method == "GET" ? serviceProxyEndpoint(path, method, body) : QByteArray{};
    const bool cleanup = method == "DELETE" && body.isEmpty() && path == "/connections";
    if (!canMutate() || (!read && !cleanup && resource != path && delay != path) ||
        path.isEmpty()) {
        return false;
    }
    auto* reply = kernel_.sendRequest(network_, path, body, method);
    reply_ = reply;
    reply->setReadBufferSize(responseLimit + 1);
    connect(reply, &QNetworkReply::readyRead, this, [reply] {
        if (reply->bytesAvailable() > responseLimit) {
            reply->abort();
        }
    });
    const auto generation = generation_;
    connect(reply, &QNetworkReply::finished, this, [this, reply, generation] {
        reply->deleteLater();
        const bool cleared = reply_ == reply;
        if (cleared) {
            reply_.clear();
        }
        const QPointer<RuntimeSession> lifetime{this};
        if (generation != generation_) {
            if (cleared) {
                Q_EMIT operationChanged();
            }
            // cppcheck-suppress knownConditionTrueFalse
            if (lifetime) {
                Q_EMIT requestFinished(0, {});
            }
            return;
        }
        const auto bytes = reply->readAll();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        Q_EMIT operationChanged();
        // cppcheck-suppress knownConditionTrueFalse
        if (!lifetime) {
            return;
        }
        Q_EMIT requestFinished(
            reply->error() == QNetworkReply::NoError && bytes.size() <= responseLimit ? status : 0,
            bytes.size() <= responseLimit ? bytes : QByteArray{});
    });
    Q_EMIT operationChanged();
    return true;
}

bool RuntimeSession::setMode(runtime_config::routing_mode mode) {
    constexpr std::array names{"rule", "global", "direct"};
    const auto index = static_cast<std::size_t>(mode);
    return index < names.size() && change({
                                       .kind = RuntimeChange::Kind::Mode,
                                       .value = QString::fromLatin1(names.at(index)),
                                       .group = {},
                                   });
}

bool RuntimeSession::selectProxy(const QString& group, const QString& name) {
    if (group.isEmpty() || name.isEmpty() || group.size() > 1024 || name.size() > 1024 ||
        group.contains(QChar::Null) || name.contains(QChar::Null)) {
        return false;
    }
    return change({.kind = RuntimeChange::Kind::Selection, .value = name, .group = group});
}

bool RuntimeSession::change(RuntimeChange requested) {
    // 模式/节点选择虽然比整份配置小，仍是“写入后回读”的事务。
    // 只有明确拒绝才能认定旧状态未变；传输失败或回读不一致都锁定为不确定，
    // 不更新 active_、selected_ 或修订号去冒充已经确认的事实。
    if (!canMutate() || active_.isEmpty()) {
        return false;
    }
    const bool mode = requested.kind == RuntimeChange::Kind::Mode;
    auto& transaction = beginTransaction(mode ? TransactionKind::Mode : TransactionKind::Selection);
    transaction.stage = TransactionStage::Apply;
    const auto target =
        mode ? QByteArray("/configs") : "/proxies/" + QUrl::toPercentEncoding(requested.group);
    const auto body =
        QJsonDocument(
            QJsonObject{{mode ? QStringLiteral("mode") : QStringLiteral("name"), requested.value}})
            .toJson(QJsonDocument::Compact);
    auto* write =
        kernel_.sendRequest(network_, target, body, mode ? QByteArray("PATCH") : QByteArray("PUT"));
    reply_ = write;
    const auto token = transactionToken();
    connect(write, &QNetworkReply::finished, this, [this, write, token, requested, mode] {
        write->deleteLater();
        if (reply_ == write) {
            reply_.clear();
        }
        if (!currentTransaction(token)) {
            return;
        }
        const auto status = write->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (write->error() != QNetworkReply::NoError || status != 204) {
            const bool rejected = status == 400 || status == 403 || status == 404 || status == 409;
            uncertain_ = !rejected;
            transaction_->result.switches.push_back({
                .stage = TransactionStage::Apply,
                .error = rejected ? TransactionError::Rejected : TransactionError::Transport,
                .httpStatus = status,
                .networkError = static_cast<int>(write->error()),
            });
            finishTransaction(
                rejected ? TransactionOutcome::Rejected : TransactionOutcome::Uncertain,
                TransactionStage::Apply,
                rejected ? TransactionError::Rejected : TransactionError::Transport,
                rejected
                    ? QStringLiteral("Runtime write rejected; confirmed state is unchanged.")
                    : QStringLiteral("Runtime write result unconfirmed; restart the session."));
            return;
        }
        transaction_->result.stage =
            mode ? TransactionStage::General : TransactionStage::FinalProxies;
        auto* read =
            kernel_.sendRequest(network_, mode ? QByteArray("/configs") : QByteArray("/proxies"));
        reply_ = read;
        read->setReadBufferSize(responseLimit + 1);
        connect(read, &QNetworkReply::readyRead, this, [read] {
            if (read->bytesAvailable() > responseLimit) {
                read->abort();
            }
        });
        connect(read, &QNetworkReply::finished, this, [this, read, token, requested, mode] {
            read->deleteLater();
            if (reply_ == read) {
                reply_.clear();
            }
            if (!currentTransaction(token)) {
                return;
            }
            const auto bytes = read->readAll();
            const auto value = QJsonDocument::fromJson(bytes).object();
            const auto actual = mode ? value.value(QStringLiteral("mode"))
                                     : value.value(QStringLiteral("proxies"))
                                           .toObject()
                                           .value(requested.group)
                                           .toObject()
                                           .value(QStringLiteral("now"));
            const bool success =
                read->error() == QNetworkReply::NoError &&
                read->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200 &&
                bytes.size() <= responseLimit && actual.isString() &&
                actual.toString() == requested.value;
            uncertain_ = !success;
            if (success && mode) {
                auto candidate = QJsonDocument::fromJson(active_.toUtf8()).object();
                candidate.insert(QStringLiteral("mode"), requested.value);
                active_ =
                    QString::fromUtf8(QJsonDocument(candidate).toJson(QJsonDocument::Compact));
                routing_ = routingMode(requested.value);
            } else if (success) {
                selected_ = selections(bytes);
            }
            if (success) {
                ++runtimeRevision_;
            } else {
                transaction_->result.switches.push_back({
                    .stage = transaction_->result.stage,
                    .error = TransactionError::Readback,
                    .httpStatus = read->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(),
                    .networkError = static_cast<int>(read->error()),
                });
            }
            finishTransaction(success ? TransactionOutcome::Applied : TransactionOutcome::Uncertain,
                              transaction_->result.stage,
                              success ? TransactionError::None : TransactionError::Readback,
                              success ? QStringLiteral("Runtime requested verified by readback.")
                                      : QStringLiteral("Readback did not confirm the runtime "
                                                       "requested; restart the session."));
        });
    });
    Q_EMIT operationChanged();
    return true;
}

bool RuntimeSession::setTunEnabled(bool enabled) {
    if (!canSetTun()) {
        return false;
    }
    static_cast<void>(beginTransaction(TransactionKind::Tun));
    issueTun(enabled);
    return true;
}

void RuntimeSession::issueTun(bool enabled) {
    // 只有提权服务能确认所属 TUN 的设备与路由。请求成功还要核对 active 布尔值；
    // 失败时先完成当前事务，再进入会话停止屏障，让系统代理恢复先于内核清理。
    if (!transaction_) {
        return;
    }
    transaction_->result.stage = TransactionStage::Tun;
    auto* reply =
        kernel_.serviceRequest(QStringLiteral("tun"), {{QStringLiteral("enable"), enabled}});
    reply_ = reply;
    if (auto* pipe = qobject_cast<PipeReply*>(reply)) {
        pipe->setTimeout(static_cast<int>(timeout_budget::tun_reply.count()));
    }
    const auto token = transactionToken();
    connect(reply, &QNetworkReply::finished, this, [this, reply, enabled, token] {
        reply->deleteLater();
        if (reply_ == reply) {
            reply_.clear();
        }
        if (!currentTransaction(token)) {
            return;
        }
        const auto value =
            QJsonDocument::fromJson(reply->readAll()).object().value(QStringLiteral("active"));
        const bool success =
            reply->error() == QNetworkReply::NoError && value.isBool() && value.toBool() == enabled;
        if (success) {
            tunActive_ = enabled;
            ++runtimeRevision_;
        }
        const bool partial = transaction_->execution == Execution::ActivationTun;
        uncertain_ = !success;
        if (!success) {
            transaction_->result.switches.push_back({
                .stage = TransactionStage::Tun,
                .error = TransactionError::Tun,
                .httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(),
                .networkError = static_cast<int>(reply->error()),
            });
        }
        const QPointer<RuntimeSession> lifetime{this};
        finishTransaction(
            tunOutcome(success, partial), TransactionStage::Tun,
            success ? TransactionError::None : TransactionError::Tun,
            success ? QStringLiteral("Configuration and service TUN verified.")
                    : QStringLiteral("TUN change failed; restoring system proxy before session "
                                     "cleanup."));
        // cppcheck-suppress knownConditionTrueFalse
        if (!lifetime) {
            return;
        }
        Q_EMIT requestFinished(success ? 200 : 0, {});
        // cppcheck-suppress knownConditionTrueFalse
        if (!lifetime) {
            return;
        }
        if (!success) {
            stop();
        }
    });
    Q_EMIT operationChanged();
}

void RuntimeSession::setLogsEnabled(bool enabled) {
    logsEnabled_ = enabled;
    if (!enabled && kernel_.serviceManaged() && kernel_.state() == KernelController::State::Ready) {
        auto const* reply = kernel_.serviceRequest(
            QStringLiteral("logs"), {{QStringLiteral("level"), QStringLiteral("off")}});
        connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
    }
    refreshStreams();
}

void RuntimeSession::refreshStreams() {
    const bool ready = kernel_.state() == KernelController::State::Ready;
    traffic_.updateContext({
        .request = ready ? kernel_.apiRequest("/traffic") : QNetworkRequest{},
        .ready = ready,
        .serviceManaged = kernel_.serviceManaged(),
    });
    if (!logsEnabled_ || suspended_ || kernel_.state() != KernelController::State::Ready) {
        logs_.stop();
        return;
    }
    if (kernel_.serviceManaged()) {
        auto const* reply = kernel_.serviceRequest(
            QStringLiteral("logs"), {{QStringLiteral("level"), QStringLiteral("debug")}});
        connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
        return;
    }
    auto logRequest = kernel_.apiRequest("/logs?level=debug");
    auto url = logRequest.url();
    url.setScheme(QStringLiteral("ws"));
    logRequest.setUrl(url);
    logs_.start(logRequest);
}

void RuntimeSession::heal() {
    // 这里只处理直接模式的环境恢复待办，并等待业务事务和代理操作空闲。
    // 清连接缓存、刷新内核 DNS 不等于重启内核；失败有限重试，不改变用户配置身份。
    if (!healPending_ || suspended_ || !online_ ||
        kernel_.state() != KernelController::State::Ready) {
        return;
    }
    if (!canMutate() || (proxy_ && proxy_->busy())) {
        healRetry_.start(std::chrono::milliseconds{500});
        return;
    }
    healPending_ = false;
    ++healAttempts_;
    network_.clearConnectionCache();
    auto* reply = kernel_.sendRequest(network_, "/cache/dns/flush", {}, "POST");
    reply_ = reply;
    healing_ = true;
    const auto generation = generation_;
    connect(reply, &QNetworkReply::finished, this, [this, reply, generation] {
        reply->deleteLater();
        const bool cleared = reply_ == reply;
        if (cleared) {
            reply_.clear();
            healing_ = false;
        }
        if (generation != generation_) {
            if (cleared) {
                Q_EMIT operationChanged();
            }
            return;
        }
        const QPointer<RuntimeSession> lifetime{this};
        const bool success = reply->error() == QNetworkReply::NoError;
        Q_EMIT environmentStatus(success ? QStringLiteral("Core DNS cache refresh acknowledged.")
                                         : QStringLiteral("Core DNS cache refresh failed."));
        // cppcheck-suppress knownConditionTrueFalse
        if (!lifetime) {
            return;
        }
        if (!success && healAttempts_ < 3) {
            healPending_ = true;
            healRetry_.start(healAttempts_ * 1000);
        }
        Q_EMIT operationChanged();
    });
    Q_EMIT operationChanged();
}
