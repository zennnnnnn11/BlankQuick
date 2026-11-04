#include <blank/runtime/runtime_session.hpp>

#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QStandardPaths>
#include <QUrl>
#include <QtConcurrentRun>
#include <array>
#include <blank/config/service_config.hpp>
#include <blank/ipc/service_client.hpp>
#include <blank/protocol/service_api.hpp>
#include <chrono>
#include <exception>
#include <utility>

namespace {
constexpr qint64 responseLimit = qint64{8} * 1024 * 1024;

struct NativeCheckInput {
    QByteArray bytes;
    NativeCheckPolicy policy = NativeCheckPolicy::RuntimeCandidate;
};

NativeCheckInput nativeCheckInput(const QJsonObject& candidate, const QByteArray& bytes) {
    const auto dns = candidate.value(QStringLiteral("dns")).toObject();
    auto profile = candidate.value(QStringLiteral("profile")).toObject();
    const auto persistence = profile.value(QStringLiteral("store-fake-ip"));
    if (dns.value(QStringLiteral("enhanced-mode")) != QStringLiteral("fake-ip") ||
        !persistence.isBool() || !persistence.toBool()) {
        return {.bytes = bytes};
    }
    profile.insert(QStringLiteral("store-fake-ip"), false);
    auto transient = candidate;
    transient.insert(QStringLiteral("profile"), profile);
    return {
        .bytes = QJsonDocument(transient).toJson(QJsonDocument::Compact),
        .policy = NativeCheckPolicy::EphemeralFakeIp,
    };
}

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

ActivationCheck prepare(ActivationInput input, const runtime_config::runtime_context& context,
                        const KernelOptions& options, const std::stop_token& stop) try {
    ActivationCheck result;
    result.diagnostics.assembly = std::move(input.assemblyDiagnostics);
    if (stop.stop_requested()) {
        result.code = TransactionError::Cancelled;
        result.error = QStringLiteral("Activation canceled.");
        return result;
    }
    auto generated =
        runtime_config::generate(input.material, input.settings, context, input.patches);
    result.diagnostics.configurationError = generated.error;
    result.diagnostics.configuration = std::move(generated.diagnostics);
    result.diagnostics.patches = std::move(generated.patch_reports);
    if (generated.error != runtime_config::configuration_error::none) {
        result.code = TransactionError::Generation;
        result.error = QStringLiteral("Configuration generation rejected the candidate.");
        return result;
    }
    result.stage = TransactionStage::ServiceAdmission;
    result.code = TransactionError::ServiceAdmission;
    if (generated.effective.tun_enabled && !options.serviceManaged) {
        result.error = QStringLiteral("TUN capture requires an installed service session.");
        return result;
    }
    if (generated.config_json.size() > std::size_t{4} * 1024 * 1024) {
        result.error = QStringLiteral("The service configuration limit is 4 MiB.");
        return result;
    }
    const auto payload = QString::fromUtf8(generated.config_json.data(),
                                           static_cast<qsizetype>(generated.config_json.size()));
    const auto admitted = compileServiceConfig(
        payload, 7890, {}, false, QString::fromStdWString(context.data_directory.native()));
    if (!admitted.error.isEmpty()) {
        result.diagnostics.serviceAdmission = admitted.error;
        result.error = QStringLiteral("Service admission rejected the candidate.");
        return result;
    }
    const auto bytes = QJsonDocument(admitted.value).toJson(QJsonDocument::Compact);
    const auto checkInput = nativeCheckInput(admitted.value, bytes);
    result.diagnostics.nativeCheckPolicy = checkInput.policy;
    result.stage = TransactionStage::NativeCheck;
    result.diagnostics.nativeCheck = runtime_config::check_with_kernel(
        std::string_view(checkInput.bytes.constData(),
                         static_cast<std::size_t>(checkInput.bytes.size())),
        {.executable = options.executable.toStdWString(), .data_directory = context.data_directory},
        stop);
    const auto error = result.diagnostics.nativeCheck->error;
    if (error != runtime_config::kernel_check_error::none) {
        result.code = error == runtime_config::kernel_check_error::cancelled
                          ? TransactionError::Cancelled
                          : TransactionError::NativeCheck;
        result.error = QStringLiteral("Native configuration check failed or was canceled.");
        return result;
    }
    result.payload = QString::fromUtf8(bytes);
    result.enableTun = generated.effective.tun_enabled;
    result.code = TransactionError::None;
    return result;
} catch (const std::exception&) {
    return {
        .payload = {},
        .error = QStringLiteral("Configuration preparation failed."),
        .code = TransactionError::Unexpected,
    };
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
      traffic_(kernel_), environment_(std::move(environment)) {
    network_.setProxy(QNetworkProxy::NoProxy);
    healRetry_.setSingleShot(true);
    connect(&healRetry_, &QTimer::timeout, this, &RuntimeSession::heal);
    connect(&checker_, &QFutureWatcher<ActivationCheck>::finished, this,
            &RuntimeSession::checkFinished);
    connect(&switcher_, &ConfigSwitcher::finished, this, &RuntimeSession::switched);
    connect(&switcher_, &ConfigSwitcher::progressChanged, this, &RuntimeSession::operationChanged,
            Qt::QueuedConnection);
    connect(&kernel_, &KernelController::stateChanged, this, &RuntimeSession::kernelChanged);
    // A derived availability update must not reenter an unfinished kernel state change.
    connect(&kernel_, &KernelController::maintenanceChanged, this,
            &RuntimeSession::operationChanged, Qt::QueuedConnection);
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
                if (changed || resumed) {
                    traffic_.environmentChanged(false);
                    refreshStreams();
                    if (proxy_) {
                        proxy_->checkEnvironment();
                    }
                    healAttempts_ = 0;
                    healPending_ = !kernel_.serviceManaged();
                }
                heal();
                Q_EMIT operationChanged();
            });
    if (!proxyOptions.helper.isEmpty() && !proxyOptions.journal.isEmpty()) {
        proxy_ = std::make_unique<SystemProxyController>(std::move(proxyOptions));
        connect(proxy_.get(), &SystemProxyController::changed, this, [this] {
            finishStop();
            Q_EMIT operationChanged();
        });
        connect(this, &RuntimeSession::operationChanged, this, &RuntimeSession::refreshProxy,
                Qt::QueuedConnection);
    }
}

RuntimeSession::~RuntimeSession() {
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
        .setTun = mutableNow && kernel_.serviceManaged() && !configLocked() && !active_.isEmpty(),
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

std::shared_ptr<const TransactionResult> RuntimeSession::lastTransaction() const noexcept {
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
    transaction_.reset();
    lastTransaction_ = result;
    Q_EMIT operationChanged();
    Q_EMIT transactionFinished(*result);
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
    environment_.start();
    if (proxy_) {
        proxy_->recover();
    }
    if (!closing() && !stopping()) {
        kernel_.start();
    }
}

void RuntimeSession::stop() {
    if (lifecycle_ == RuntimeLifecycle::Closed) {
        return;
    }
    ++generation_;
    lifecycle_ = closing() ? RuntimeLifecycle::ShuttingDown : RuntimeLifecycle::Stopping;
    static_cast<void>(cancellation_.request_stop());
    if (reply_) {
        reply_->abort();
    }
    switcher_.cancel();
    cancelTransaction();
    if (proxy_) {
        proxy_->setEnabled(false);
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
    // The native checker can still hold provider/GEO files in the core home.
    // Finish its cancellation before the owned core removes that directory.
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
    const bool closingRequested = closing();
    lifecycle_ = closingRequested ? RuntimeLifecycle::Closed : RuntimeLifecycle::Open;
    Q_EMIT operationChanged();
    if (closingRequested) {
        Q_EMIT shutdownFinished();
    }
}

bool RuntimeSession::canMutate() const noexcept {
    return kernel_.state() == KernelController::State::Ready && !busy() && !stopping() &&
           !uncertain_ && !kernel_.maintenance();
}

bool RuntimeSession::activate(ActivationInput input) {
    return submitActivation(std::move(input)).accepted();
}

ActivationSubmission RuntimeSession::submitActivation(ActivationInput input) {
    const auto rejection = activationRejection();
    if (rejection != ActivationRejection::None) {
        return {.rejection = rejection};
    }
    auto& transaction = beginTransaction(TransactionKind::Activation);
    const auto requestId = transaction.requestId;
    try {
        transaction.identity = std::make_shared<const ActivationIdentity>(
            ActivationIdentity{.requestId = requestId, .composition = std::move(input.identity)});
        transaction.stage = TransactionStage::Generation;
        cancellation_ = std::stop_source{};
        checkGeneration_ = generation_;
        auto options = options_;
        options.serviceManaged = kernel_.serviceManaged();
        checker_.setFuture(QtConcurrent::run(prepare, std::move(input), context(), options,
                                             cancellation_.get_token()));
    } catch (const std::exception&) {
        finishTransaction(
            TransactionOutcome::Rejected, TransactionStage::Generation,
            TransactionError::Unexpected,
            QStringLiteral("Invalid activation context or unavailable worker resources."));
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
    Q_EMIT operationChanged();
    finishStop();
}

void RuntimeSession::switched(const SwitchResult& result) {
    if (!transaction_) {
        return;
    }
    uncertain_ = result.outcome == TransactionOutcome::Uncertain;
    transaction_->result.switches = result.diagnostics;
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
    if (kernel_.state() != KernelController::State::Ready) {
        ++generation_;
        static_cast<void>(cancellation_.request_stop());
        cancelTransaction();
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
    }
    refreshStreams();
    Q_EMIT operationChanged();
}

bool RuntimeSession::request(const QByteArray& path, const QByteArray& method,
                             const QByteArray& body) {
    // Admission is identical in user and service mode. Config, selection and TUN
    // mutations have dedicated transactions and cannot bypass the session cache.
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
        if (reply_ == reply) {
            reply_.clear();
        }
        if (generation != generation_) {
            return;
        }
        const auto bytes = reply->readAll();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        Q_EMIT operationChanged();
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
    if (!canMutate() || !kernel_.serviceManaged() || configLocked() || active_.isEmpty()) {
        return false;
    }
    static_cast<void>(beginTransaction(TransactionKind::Tun));
    issueTun(enabled);
    return true;
}

void RuntimeSession::issueTun(bool enabled) {
    if (!transaction_) {
        return;
    }
    transaction_->result.stage = TransactionStage::Tun;
    auto* reply =
        kernel_.serviceRequest(QStringLiteral("tun"), {{QStringLiteral("enable"), enabled}});
    reply_ = reply;
    if (auto* pipe = qobject_cast<PipeReply*>(reply)) {
        pipe->setTimeout(350000);
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
        finishTransaction(
            tunOutcome(success, partial), TransactionStage::Tun,
            success ? TransactionError::None : TransactionError::Tun,
            success ? QStringLiteral("Configuration and service TUN verified.")
                    : QStringLiteral("TUN activation failed; session cleanup requested."));
        Q_EMIT requestFinished(success ? 200 : 0, {});
        if (!success) {
            kernel_.stop();
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
        if (reply_ == reply) {
            reply_.clear();
            healing_ = false;
        }
        if (generation != generation_) {
            return;
        }
        const bool success = reply->error() == QNetworkReply::NoError;
        Q_EMIT environmentStatus(success ? QStringLiteral("Core DNS cache refresh acknowledged.")
                                         : QStringLiteral("Core DNS cache refresh failed."));
        if (!success && healAttempts_ < 3) {
            healPending_ = true;
            healRetry_.start(healAttempts_ * 1000);
        }
        Q_EMIT operationChanged();
    });
    Q_EMIT operationChanged();
}
