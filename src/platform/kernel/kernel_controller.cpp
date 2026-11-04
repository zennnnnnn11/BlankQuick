#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QWinEventNotifier>
#include <QtConcurrentRun>
#include <blank/ipc/service_client.hpp>
#include <blank/protocol/service_api.hpp>
#include <blank/runtime/kernel_controller.hpp>
#include <blank/shared/timeouts.hpp>
#include <chrono>
#include <utility>

KernelController::KernelController(KernelOptions options, QObject* parent)
    : QObject(parent), options_(std::move(options)) {
    published_ = currentStatus();
    network_.setProxy(QNetworkProxy::NoProxy);
    poll_.setInterval(std::chrono::milliseconds{50});
    poll_.setSingleShot(true);
    deadline_.setSingleShot(true);
    connect(&poll_, &QTimer::timeout, this, &KernelController::probe);
    connect(&deadline_, &QTimer::timeout, this, [this] { fail(tr("内核启动超时")); });
    connect(&launch_, &QFutureWatcher<KernelLaunch>::finished, this,
            &KernelController::launchFinished);
    connect(&listener_, &QFutureWatcher<ListenerCheck>::finished, this,
            &KernelController::listenerChecked);
    connect(&cleanup_, &QFutureWatcher<QString>::finished, this,
            &KernelController::cleanupFinished);
}

KernelController::~KernelController() {
    QObject::disconnect(this, nullptr, nullptr, nullptr);
    if (launchPending_) {
        auto pending = launch_.future();
        const auto helper = options_.signalHelper;

        static_cast<void>(QtConcurrent::run([pending, helper]() mutable {
            pending.waitForFinished();
            if (pending.resultCount() != 0) {
                const auto result = pending.takeResult();
                if (result.kernel) {
                    return result.kernel->stop(helper);
                }
            }
            return QString{};
        }));
    } else if (kernel_ && !cleanup_.isRunning()) {
        const auto kernel = kernel_;
        const auto helper = options_.signalHelper;

        static_cast<void>(QtConcurrent::run([kernel, helper] { return kernel->stop(helper); }));
    }
}

KernelController::State KernelController::state() const noexcept {
    return state_;
}

const QString& KernelController::version() const noexcept {
    return version_;
}

quint32 KernelController::processId() const noexcept {
    if (options_.serviceManaged) {
        return servicePid_;
    }
    return kernel_ ? kernel_->processId() : 0;
}

quint16 KernelController::port() const noexcept {
    return kernel_ ? kernel_->port() : 0;
}

QString KernelController::statusText() const {
    switch (state_) {
    case State::Stopped:
        return tr("内核已停止");
    case State::Starting:
        return tr("内核正在启动");
    case State::WaitingForApi:
        return tr("正在等待内核接口");
    case State::Ready:
        return tr("内核已连接 / 就绪：%1").arg(version_);
    case State::Stopping:
        return tr("内核正在停止");
    case State::Failed:
        return tr("内核错误：%1").arg(error_);
    }
    return {};
}

QString KernelController::initialConfiguration() const {
    return kernel_ ? kernel_->initialConfiguration() : QString{};
}

QString KernelController::homeDirectory() const {
    return kernel_ ? kernel_->workingDirectory() : QString{};
}

QString KernelController::sessionSecret() const {
    return kernel_ ? kernel_->secret() : QString{};
}

QString KernelController::controllerAddress() const {
    return kernel_ ? QStringLiteral("127.0.0.1:%1").arg(kernel_->port()) : QString{};
}

KernelController::PublishedStatus KernelController::currentStatus() const {
    return {
        .state = state_,
        .resources = resources(),
        .pid = processId(),
        .version = version_,
        .error = error_,
        .maintenance = maintenance_,
        .serviceManaged = options_.serviceManaged,
    };
}

void KernelController::publishStatus() {
    const auto current = currentStatus();
    if (published_ == current) {
        return;
    }

    published_ = current;
    Q_EMIT statusChanged();

    if (notifiedState_ != state_) {
        notifiedState_ = state_;
        Q_EMIT stateChanged();
    }
    if (notifiedMaintenance_ != maintenance_) {
        notifiedMaintenance_ = maintenance_;
        Q_EMIT maintenanceChanged();
    }
}

void KernelController::setState(State nextState) {
    state_ = nextState;
    if (nextState != State::Ready) {
        maintenance_ = false;
    }
    publishStatus();
}

void KernelController::start() {
    if (quitting_ || resources() != Resources::Released ||
        (state_ != State::Stopped && state_ != State::Failed)) {
        return;
    }
    ++generation_;
    error_.clear();
    version_.clear();
    pendingVersion_.clear();
    deadline_.start(options_.startupTimeoutMs);
    launchPending_ = true;
    if (options_.serviceManaged) {
        launchPending_ = false;
        serviceResources_ = Resources::Pending;
        connectService();
        const auto generation = generation_;
        setState(State::Starting);
        if (generation != generation_ || state_ != State::Starting) {
            return;
        }
        auto* reply = service_->request(QStringLiteral("start"));
        connect(reply, &QNetworkReply::finished, this, [this, reply, generation] {
            reply->deleteLater();
            if (generation != generation_ || state_ != State::Starting) {
                return;
            }
            const auto object = QJsonDocument::fromJson(reply->readAll()).object();
            const auto pid = object.value(QStringLiteral("pid")).toInteger();
            const auto version = object.value(QStringLiteral("version")).toString();
            if (reply->error() != QNetworkReply::NoError || pid <= 0 ||
                std::cmp_greater(pid, UINT32_MAX) || version.isEmpty() || version.size() > 128) {

                deadline_.stop();
                error_ = tr("服务无法启动内核：%1").arg(reply->errorString());
                serviceResources_ = Resources::Uncertain;
                setState(State::Failed);
                return;
            }
            servicePid_ = static_cast<quint32>(pid);
            serviceResources_ = Resources::Owned;
            version_ = version;
            maintenance_ = object.value(QStringLiteral("maintenance")).toBool();
            deadline_.stop();
            setState(State::Ready);
            if (generation == generation_ && state_ == State::Ready) {
                Q_EMIT serviceEnvironment(object.value(QStringLiteral("environment")).toString());
            }
        });
        return;
    }
    launch_.setFuture(QtConcurrent::run(launchKernel, options_));
    setState(State::Starting);
}

void KernelController::launchFinished() {
    launchPending_ = false;
    const auto result = launch_.future().takeResult();
    kernel_ = result.kernel;
    if (state_ == State::Stopping) {
        publishStatus();
        beginCleanup();
        return;
    }
    if (!kernel_) {
        fail(result.error);
        return;
    }
    processNotifier_ = std::make_unique<QWinEventNotifier>(kernel_->processHandle());
    connect(processNotifier_.get(), &QWinEventNotifier::activated, this, [this] {
        processNotifier_->setEnabled(false);
        fail(tr("内核意外退出"));
    });
    setState(State::WaitingForApi);
    if (state_ == State::WaitingForApi) {
        probe();
    }
}

void KernelController::probe() {
    if (state_ == State::WaitingForApi && kernel_) {
        if (listener_.isRunning()) {
            poll_.start();
            return;
        }
        listenerGeneration_ = generation_;
        listener_.setFuture(
            QtConcurrent::run(checkKernelListener, kernel_->port(), kernel_->processId()));
    }
}

void KernelController::listenerChecked() {
    if (state_ != State::WaitingForApi) {
        return;
    }
    const auto result = listener_.result();
    if (listenerGeneration_ != generation_) {
        poll_.start();
        return;
    }
    if (!result.error.isEmpty()) {
        fail(result.error);
    } else if (result.owned) {
        if (pendingVersion_.isEmpty()) {
            requestVersion();
        } else {
            deadline_.stop();
            version_ = std::exchange(pendingVersion_, {});
            setState(State::Ready);
        }
    } else if (!pendingVersion_.isEmpty()) {
        fail(tr("内核监听端口的归属发生变化"));
    } else {
        poll_.start();
    }
}

void KernelController::requestVersion() {
    auto request = apiRequest("/version");
    request.setTransferTimeout(1500);
    auto* reply = network_.get(request);
    reply->setReadBufferSize(65537);
    reply_ = reply;
    const auto generation = generation_;
    connect(reply, &QNetworkReply::readyRead, this, [reply] {
        if (reply->bytesAvailable() > 65536) {
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, generation] { versionFinished(reply, generation); });
}

QNetworkRequest KernelController::apiRequest(const QByteArray& target) const {
    if (options_.serviceManaged) {
        return {};
    }
    QNetworkRequest request(
        QUrl::fromEncoded("http://127.0.0.1:" + QByteArray::number(port()) + target));
    request.setRawHeader("Authorization", "Bearer " + kernel_->secret().toUtf8());
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    return request;
}

void KernelController::versionFinished(QNetworkReply* reply, quint64 generation) {
    reply->deleteLater();
    if (generation != generation_ || state_ != State::WaitingForApi) {
        return;
    }
    reply_.clear();
    if (reply->error() != QNetworkReply::NoError) {
        poll_.start();
        return;
    }
    const auto document = QJsonDocument::fromJson(reply->readAll());
    const auto reportedVersion = document.object().value(QStringLiteral("version")).toString();
    if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200 ||
        !document.isObject() || reportedVersion.isEmpty() || reportedVersion.size() > 128) {
        fail(tr("内核版本响应无效"));
        return;
    }

    pendingVersion_ = reportedVersion;
    listenerGeneration_ = generation_;
    listener_.setFuture(
        QtConcurrent::run(checkKernelListener, kernel_->port(), kernel_->processId()));
}

void KernelController::fail(const QString& error) {
    if (state_ == State::Stopping || state_ == State::Stopped || state_ == State::Failed) {
        return;
    }
    error_ = error;
    stop();
}

void KernelController::stop() {
    if (state_ == State::Stopped || state_ == State::Stopping) {
        return;
    }
    ++generation_;
    poll_.stop();
    deadline_.stop();
    if (reply_) {
        reply_->abort();
        reply_.clear();
    }
    processNotifier_.reset();
    setState(State::Stopping);
    if (options_.serviceManaged) {
        const auto generation = generation_;
        auto* reply = service_->request(QStringLiteral("stop"));
        connect(reply, &QNetworkReply::finished, this, [this, reply, generation] {
            reply->deleteLater();
            if (generation != generation_ || state_ != State::Stopping) {
                return;
            }
            if (reply->error() != QNetworkReply::NoError) {
                error_ = tr("尚未确认服务清理完成：%1").arg(reply->errorString());
                if (serviceResources_ != Resources::Released) {
                    serviceResources_ = Resources::Uncertain;
                }
                setState(State::Failed);
                return;
            }
            servicePid_ = 0;
            serviceResources_ = Resources::Released;
            finishStopped();
        });
        return;
    }
    if (!launchPending_) {
        beginCleanup();
    }
}

bool KernelController::serviceManaged() const noexcept {
    return options_.serviceManaged;
}

bool KernelController::maintenance() const noexcept {
    return maintenance_;
}

KernelController::Resources KernelController::resources() const noexcept {
    if (options_.serviceManaged) {
        return serviceResources_;
    }
    if (launchPending_) {
        return Resources::Pending;
    }
    if (!kernel_) {
        return Resources::Released;
    }
    return state_ == State::Failed ? Resources::Uncertain : Resources::Owned;
}

void KernelController::setMaintenance(bool enabled) {
    maintenance_ = enabled;
    publishStatus();
}

void KernelController::connectService() {
    if (service_) {
        return;
    }
    service_ = std::make_unique<ServiceClient>();
    connect(service_.get(), &ServiceClient::traffic, this, &KernelController::serviceTraffic);
    connect(service_.get(), &ServiceClient::logs, this, &KernelController::serviceLogs);
    connect(service_.get(), &ServiceClient::logState, this, &KernelController::serviceLogState);
    connect(service_.get(), &ServiceClient::environment, this,
            [this](const QString& message, bool repairing) {
                setMaintenance(repairing);
                Q_EMIT serviceEnvironment(message);
            });
    connect(service_.get(), &ServiceClient::coreStopped, this, [this](const QString& message) {
        servicePid_ = 0;
        serviceResources_ = Resources::Released;
        if (options_.serviceManaged && state_ != State::Stopping && state_ != State::Stopped) {
            error_ = message.isEmpty() ? tr("服务内核已停止，网络资源已清理") : message;
            setState(State::Failed);
        } else {
            publishStatus();
        }
    });
    connect(service_.get(), &ServiceClient::lost, this, [this](const QString& message) {
        if (state_ != State::Stopped && state_ != State::Stopping) {
            error_ = message;
            if (serviceResources_ != Resources::Released) {
                serviceResources_ = Resources::Uncertain;
            }
            setState(State::Failed);
        }
    });
}

void KernelController::useService(bool enabled) {
    if ((state_ == State::Stopped || state_ == State::Failed) &&
        resources() == Resources::Released) {
        options_.serviceManaged = enabled;
        publishStatus();
    }
}

QNetworkReply* KernelController::serviceRequest(const QString& operation,
                                                const QJsonObject& arguments) {
    connectService();
    return service_->request(operation, arguments);
}

QNetworkReply* KernelController::sendRequest(QNetworkAccessManager& network,
                                             const QByteArray& target, const QByteArray& body,
                                             const QByteArray& method) {
    if (options_.serviceManaged) {
        auto* reply =
            serviceRequest(QStringLiteral("api"),
                           {
                               {QStringLiteral("path"), QString::fromLatin1(target)},
                               {QStringLiteral("method"), QString::fromLatin1(method)},
                               {QStringLiteral("body"), QString::fromLatin1(body.toBase64())},
                           });
        const auto timeout = coreRequestTimeout(target, method, 0);
        if (timeout > 0) {
            if (auto* pipe = qobject_cast<PipeReply*>(reply)) {
                const auto budget = target == "/configs?force=true" && method == "PUT"
                                        ? timeout_budget::config_reply
                                        : std::chrono::milliseconds{timeout} +
                                              timeout_budget::service_overhead +
                                              timeout_budget::ipc_overhead;
                pipe->setTimeout(static_cast<int>(budget.count()));
            }
        }
        return reply;
    }
    auto request = apiRequest(target);
    request.setTransferTimeout(
        coreRequestTimeout(target, method, static_cast<int>(timeout_budget::request.count())));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    return method == "GET" ? network.get(request)
                           : network.sendCustomRequest(request, method, body);
}

void KernelController::beginCleanup() {
    if (!kernel_) {
        finishStopped();
        return;
    }
    const auto kernel = kernel_;
    const auto helper = options_.signalHelper;
    cleanup_.setFuture(QtConcurrent::run([kernel, helper] { return kernel->stop(helper); }));
}

void KernelController::cleanupFinished() {
    const auto result = cleanup_.result();
    if (!result.isEmpty()) {
        error_ = result;
        setState(State::Failed);
        return;
    }
    kernel_.reset();
    finishStopped();
}

void KernelController::finishStopped() {
    setState(error_.isEmpty() ? State::Stopped : State::Failed);
    if (quitting_) {
        Q_EMIT shutdownFinished();
    }
}

void KernelController::shutdown() {
    quitting_ = true;
    if ((state_ == State::Stopped || state_ == State::Failed) &&
        resources() == Resources::Released) {
        Q_EMIT shutdownFinished();
    } else {
        stop();
    }
}
