#include "service_host.hpp"

#include <AclAPI.h>
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMetaEnum>
#include <QNetworkProxy>
#include <QUrlQuery>
#include <QUuid>
#include <QtConcurrentRun>
#include <blank/protocol/service_api.hpp>
#include <blank/shared/timeouts.hpp>
#include <chrono>
#include <exception>
#include <system_error>
#include <utility>

namespace {
TunRequirements requirementsFor(const QJsonObject& config) {
    const auto tun = config.value(QStringLiteral("tun")).toObject();
    bool defaults = tun.value(QStringLiteral("auto-route")).toBool(true);
    for (const auto* key : {
             "route-address",
             "route-address-set",
             "route-exclude-address",
             "route-exclude-address-set",
             "inet4-route-address",
             "inet6-route-address",
             "inet4-route-exclude-address",
             "inet6-route-exclude-address",
         }) {
        defaults = defaults && tun.value(QString::fromLatin1(key)).toArray().isEmpty();
    }
    return {.ipv6 = config.value(QStringLiteral("ipv6")).toBool(), .defaultRoutes = defaults};
}

void allowProcessObservation(HANDLE process, const QString& logonSid) {
    const auto descriptor = securityDescriptor(
        QStringLiteral("D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;0x00101000;;;%1)").arg(logonSid));
    ACL* acl = nullptr;
    BOOL present = FALSE;
    BOOL inherited = FALSE;
    blank::windows::check(GetSecurityDescriptorDacl(descriptor.get(), &present, &acl, &inherited) !=
                              FALSE &&
                          (present != 0));
    const auto status = SetSecurityInfo(
        process, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        nullptr, nullptr, acl, nullptr);
    if (status != ERROR_SUCCESS) {
        throw std::system_error(static_cast<int>(status), std::system_category());
    }
}
} // namespace

ServiceHost::ServiceHost(QObject* parent) : QObject(parent) {
    const auto root = QCoreApplication::applicationDirPath();
    const auto data = serviceDataDirectory();
    options_.executable = root + QStringLiteral("/core/mihomo.exe");
    options_.signalHelper = root + QStringLiteral("/kernel_signal.exe");
    options_.workRoot = data;
    journal_ = data + QStringLiteral("/tun.json");
    network_.setProxy(QNetworkProxy::NoProxy);
    connect(&logs_, &CoreLogSocket::message, this, [this](const QString& text) {
        pipe_.sendEvent({
            {QStringLiteral("event"), QStringLiteral("logs")},
            {QStringLiteral("value"), text},
        });
    });
    connect(&logs_, &CoreLogSocket::stateChanged, this,
            [this](bool connected, const QString& status) {
                pipe_.sendEvent({
                    {QStringLiteral("event"), QStringLiteral("log-state")},
                    {QStringLiteral("connected"), connected},
                    {QStringLiteral("message"), status},
                });
            });
    traffic_.setProxy(QNetworkProxy::NoProxy);
    traffic_.setMaxAllowedIncomingFrameSize(4096);
    traffic_.setMaxAllowedIncomingMessageSize(4096);
    trafficRetry_.setSingleShot(true);
    poll_.setSingleShot(true);
    deadline_.setSingleShot(true);
    environmentRetry_.setSingleShot(true);
    trafficWatchdog_.setSingleShot(true);
    connect(&environment_, &EnvironmentMonitor::observed, this, &ServiceHost::environmentObserved);
    connect(&environment_, &EnvironmentMonitor::failed, this, [this](const QString& error) {
        reportEnvironment(QStringLiteral("环境监听/观察失败：%1").arg(error));
    });
    connect(&environmentRetry_, &QTimer::timeout, this, &ServiceHost::checkEnvironment);
    connect(&trafficWatchdog_, &QTimer::timeout, this, &ServiceHost::openTraffic);
    connect(&health_, &QFutureWatcher<TunHealth>::finished, this,
            &ServiceHost::environmentHealthFinished);
    connect(&pipe_, &SecurePipe::message, this, &ServiceHost::received);
    connect(&pipe_, &SecurePipe::closed, this, [this] { stop(); });
    connect(&pipe_, &SecurePipe::failed, this, [this](const QString& error) {
        Q_EMIT fatal(error);
        shutdown();
    });
    connect(&launch_, &QFutureWatcher<KernelLaunch>::finished, this, &ServiceHost::launchFinished);
    connect(&listener_, &QFutureWatcher<ListenerCheck>::finished, this, [this] {
        const auto token = std::exchange(listenerWork_, std::nullopt);
        const auto result = listener_.future().takeResult();
        if (!token || stopping_ || !core_ || token->kernelEpoch != generation_ ||
            token->operation != operation_) {
            return;
        }
        if (!result.error.isEmpty()) {
            stop(activeId_, result.error);
            return;
        }
        if (!result.owned) {
            poll_.start(std::chrono::milliseconds{100});
            return;
        }
        if (operation_ == Operation::Starting) {
            sendHttp("/version");
        } else if (operation_ == Operation::VerifyVersion) {
            operation_ = Operation::InitialGeneral;
            sendHttp("/configs");
        }
    });
    connect(&stopper_, &QFutureWatcher<QString>::finished, this, [this] {
        stopWork_.reset();
        const auto result = stopper_.future().takeResult();
        if (!result.isEmpty()) {
            stopError_ = result;
            blocked_ = true;
            finish(500, "{}", result);
            Q_EMIT fatal(result);
            return;
        }
        core_.reset();
        cleanup(Cleanup::Stop);
    });
    connect(&cleaner_, &QFutureWatcher<QString>::finished, this, &ServiceHost::cleanupFinished);
    connect(&planner_, &QFutureWatcher<TunPlan>::finished, this, [this] {
        const auto token = std::exchange(plannerWork_, std::nullopt);
        const auto result = planner_.future().takeResult();
        if (stopping_) {
            stopProcess();
            return;
        }
        if (!token || token->kernelEpoch != generation_ || token->tunEpoch != tunGeneration_ ||
            token->operation != operation_) {
            stop(activeId_, QStringLiteral("Stale TUN planning result requires cleanup."));
            return;
        }
        if (!result.error.isEmpty()) {
            if (repair_ == Repair::InProgress) {
                stop({}, result.error);
                return;
            }
            finish(409, "{}", result.error);
            return;
        }
        plan_ = result;
        pendingConfig_ = runtimeConfig_;
        auto tun = pendingConfig_.value(QStringLiteral("tun")).toObject();
        tun.insert(QStringLiteral("enable"), true);
        tun.insert(QStringLiteral("device"), plan_.device);
        pendingConfig_.insert(QStringLiteral("tun"), tun);
        operation_ = Operation::Enable;
        // DNS 常驻，TUN 切换只提交 tun 字段，不重载策略组和节点选择。
        sendHttp("/configs", "PATCH",
                 QJsonDocument(QJsonObject{{QStringLiteral("tun"), tun}}).toJson());
    });
    connect(&observer_, &QFutureWatcher<TunPlan>::finished, this, [this] {
        const auto token = std::exchange(observerWork_, std::nullopt);
        const auto result = observer_.future().takeResult();
        if (stopping_) {
            stopProcess();
            return;
        }
        if (!token || token->kernelEpoch != generation_ || token->tunEpoch != tunGeneration_ ||
            token->operation != operation_) {
            return;
        }
        if (operation_ == Operation::VerifyConfigTun) {
            if (!result.error.isEmpty() || result.device != plan_.device ||
                result.guid != plan_.guid || result.luid != plan_.luid) {
                finish(503, "{}",
                       QStringLiteral(
                           "TUN identity or owned routes failed configuration verification: ") +
                           result.error);
            } else {
                plan_ = result;
                finish(204);
            }
            return;
        }
        if (operation_ != Operation::VerifyTun) {
            return;
        }
        if (!result.error.isEmpty()) {
            poll_.start(std::chrono::milliseconds{100});
            return;
        }
        plan_ = result;
        tunActive_ = true;
        finish(200, "{\"active\":true}");
        if (repair_ == Repair::InProgress) {
            repair_ = Repair::Exhausted;
            ++tunRepairs_;
            healthFailure_.invalidate();
            reportEnvironment(QStringLiteral("TUN 网卡与路由已恢复"));
        }
    });
    connect(&compiler_, &QFutureWatcher<ServiceConfig>::finished, this, [this] {
        const auto token = std::exchange(compilerWork_, std::nullopt);
        const auto result = compiler_.future().takeResult();
        if (stopping_) {
            stopProcess();
            return;
        }
        if (!token || token->kernelEpoch != generation_ || operation_ != Operation::Compile) {
            return;
        }
        if (!result.error.isEmpty()) {
            finish(400, "{}", result.error);
            return;
        }
        pendingConfig_ = result.value;
        operation_ = Operation::ConfigApply;
        ++tunGeneration_;
        sendHttp(
            "/configs?force=true", "PUT",
            QJsonDocument(QJsonObject{
                              {
                                  QStringLiteral("payload"),
                                  QString::fromUtf8(
                                      QJsonDocument(result.value).toJson(QJsonDocument::Compact)),
                              },
                          })
                .toJson(QJsonDocument::Compact));
    });
    connect(&poll_, &QTimer::timeout, this, [this] {
        if (operation_ == Operation::Starting || operation_ == Operation::VerifyVersion) {
            readVersion();
        } else if (operation_ == Operation::VerifyTun) {
            verifyTun();
        }
    });
    connect(&deadline_, &QTimer::timeout, this, [this] {
        if ((operation_ == Operation::Api || operation_ == Operation::ConfigApply ||
             operation_ == Operation::EnvironmentDns) &&
            reply_) {
            // Sleep can expire an HTTP deadline before its disconnect event is delivered.
            // A failed read/cache refresh does not prove the owned TUN is damaged.
            reply_->abort();
            return;
        }
        if (operation_ == Operation::Compile || operation_ == Operation::VerifyConfigTun) {
            finish(504, "{}",
                   QStringLiteral("Configuration processing timed out; the client must "
                                  "verify or roll back."));
            return;
        }
        stop(activeId_,
             QStringLiteral("Service operation timed out; releasing the owned network resources."));
    });
    connect(&trafficRetry_, &QTimer::timeout, this, &ServiceHost::openTraffic);
    connect(&traffic_, &QWebSocket::textMessageReceived, this, [this](const QString& message) {
        const auto sample = QJsonDocument::fromJson(message.toUtf8()).object();
        if (sample.value(QStringLiteral("up")).toInteger(-1) < 0 ||
            sample.value(QStringLiteral("down")).toInteger(-1) < 0) {
            traffic_.abort();
            return;
        }
        trafficWatchdog_.start(std::chrono::milliseconds{5000});
        pipe_.send({
            {QStringLiteral("event"), QStringLiteral("traffic")},
            {QStringLiteral("value"), message},
        });
    });
    connect(&traffic_, &QWebSocket::binaryMessageReceived, this,
            [this](const QByteArray&) { traffic_.abort(); });
    connect(&traffic_, &QWebSocket::disconnected, this, [this] {
        if (core_ && !stopping_ && !suspended_) {
            trafficRetry_.start(std::chrono::milliseconds{1000});
        }
    });
}

ServiceHost::~ServiceHost() {
    QObject::disconnect(&environment_, nullptr, this, nullptr);
    environmentRetry_.stop();
    trafficWatchdog_.stop();
    QObject::disconnect(&traffic_, nullptr, this, nullptr);
    QObject::disconnect(&pipe_, nullptr, this, nullptr);
    trafficRetry_.stop();
    traffic_.abort();
    processEvent_.reset();
}

void ServiceHost::initialize() {
    if (!runningAsSystem()) {
        throw std::runtime_error("Installed service mode requires LocalSystem.");
    }
    const auto root = installedServiceDirectory();
    if (QCoreApplication::applicationDirPath().compare(root, Qt::CaseInsensitive) != 0) {
        throw std::runtime_error("The service must run from its protected installation.");
    }
    verifyTrustedPath(options_.executable);
    verifyTrustedPath(options_.signalHelper);
    verifyTrustedPath(root + QStringLiteral("/blank_quick_service.exe"));
    verifyTrustedPath(root + QStringLiteral("/core/wintun.dll"));
    protectDirectory(serviceDataDirectory(), false);
    environment_.start();
    cleanup(Cleanup::Startup);
}

void ServiceHost::received(const QJsonObject& frame) try {
    const auto id = frame.value(QStringLiteral("id")).toString();
    const auto command = frame.value(QStringLiteral("op")).toString();
    const auto arguments = frame.value(QStringLiteral("args")).toObject();
    if (id.isEmpty() || id.size() > 32 || frame.size() != 3 ||
        !frame.value(QStringLiteral("args")).isObject()) {
        pipe_.disconnectPeer();
        return;
    }
    if ((command == QStringLiteral("stop") || command == QStringLiteral("start") ||
         command == QStringLiteral("status") || command == QStringLiteral("traffic") ||
         command == QStringLiteral("traffic-stop")) &&
        !arguments.isEmpty()) {
        respond(
            id, 400, "{}",
            QStringLiteral("Lifecycle commands do not accept client process or path arguments."));
        return;
    }
    if (command == QStringLiteral("stop")) {
        stop(id);
        return;
    }
    if (command == QStringLiteral("traffic-stop")) {
        trafficRequested_ = false;
        trafficWatchdog_.stop();
        trafficRetry_.stop();
        traffic_.abort();
        respond(id, 200, "{}");
        return;
    }
    if (command == QStringLiteral("traffic") && core_ && !stopping_ && !suspended_) {
        trafficRequested_ = true;
        if (traffic_.state() == QAbstractSocket::UnconnectedState && !trafficRetry_.isActive()) {
            openTraffic();
        }
        respond(id, 200, "{}");
        return;
    }
    if (command == QStringLiteral("logs") && core_ && !stopping_ && !suspended_) {
        const auto level = arguments.value(QStringLiteral("level")).toString();
        if (arguments.size() != 1 ||
            (level != QStringLiteral("off") && level != QStringLiteral("debug") &&
             level != QStringLiteral("info") && level != QStringLiteral("warning") &&
             level != QStringLiteral("error"))) {
            respond(id, 400, "{}", QStringLiteral("Invalid log level."));
            return;
        }
        if (level == QStringLiteral("off")) {
            logLevel_.clear();
            logs_.stop();
            respond(id, 200, "{}");
            return;
        }
        // This core only retires an old log subscriber on a matching log write.
        // Keep one debug subscription and let the client filter levels without reopening it.
        logLevel_ = QStringLiteral("debug");
        openLogs();
        respond(id, 200,
                QJsonDocument(QJsonObject{{QStringLiteral("connected"), logs_.connected()}})
                    .toJson(QJsonDocument::Compact));
        return;
    }
    if (command == QStringLiteral("api") && arguments.size() == 3 && core_ && !stopping_ &&
        !blocked_ && !serviceStopping_ && !suspended_ &&
        arguments.value(QStringLiteral("path")) == QStringLiteral("/connections") &&
        arguments.value(QStringLiteral("method")) == QStringLiteral("GET") &&
        arguments.value(QStringLiteral("body")) == QString()) {
        readConnections(id);
        return;
    }
    if (command == QStringLiteral("status")) {
        respond(id, 200, information());
        return;
    }
    if (blocked_ || serviceStopping_ || operation_ != Operation::Idle) {
        respond(id, 409, "{}", QStringLiteral("Service is busy or cleanup is unresolved."));
        return;
    }
    activeId_ = id;
    if (command == QStringLiteral("start")) {
        if (core_) {
            finish(200, information());
            return;
        }
        operation_ = Operation::Starting;
        repair_ = Repair::Available;
        healthFailure_.invalidate();
        healthAttempts_ = 0;
        dnsAttempts_ = 0;
        ++generation_;
        deadline_.start(std::chrono::milliseconds{15000});
        startWork(launch_, launchWork_,
                  [this] { return QtConcurrent::run(launchKernel, options_); });
    } else if (command == QStringLiteral("api") && core_) {
        deadline_.start(timeout_budget::request + timeout_budget::service_overhead);
        forwardApi(arguments);
    } else if (command == QStringLiteral("tun") && core_ && arguments.size() == 1 &&
               arguments.value(QStringLiteral("enable")).isBool()) {
        const bool enabled = arguments.value(QStringLiteral("enable")).toBool();
        if (enabled == tunActive_) {
            finish(200, information());
            return;
        }
        deadline_.start(timeout_budget::tun);
        if (enabled) {
            repair_ = Repair::Available;
            healthFailure_.invalidate();
            enableTun();
        } else {
            operation_ = Operation::Disable;
            ++tunGeneration_;
            sendHttp("/configs", "PATCH", R"({"tun":{"enable":false}})");
        }
    } else {
        finish(400, "{}", QStringLiteral("Unsupported service operation or core not ready."));
    }
} catch (const std::exception& error) {
    stop(activeId_, QString::fromUtf8(error.what()));
}

void ServiceHost::launchFinished() try {
    launchWork_.reset();
    const auto result = launch_.future().takeResult();
    core_ = result.kernel;
    if (stopping_) {
        stopProcess();
        return;
    }
    if (!core_) {
        finish(500, "{}", result.error);
        return;
    }
    allowProcessObservation(core_->processHandle(), pipe_.identity().logonSid);
    processEvent_ = std::make_unique<QWinEventNotifier>(core_->processHandle());
    connect(processEvent_.get(), &QWinEventNotifier::activated, this, [this] {
        processEvent_->setEnabled(false);
        stop({}, QStringLiteral("The service-owned core exited unexpectedly."));
    });
    readVersion();
} catch (const std::exception& error) {
    stop(activeId_, QString::fromUtf8(error.what()));
}

void ServiceHost::readVersion() {
    if (listenerWork_) {
        poll_.start(std::chrono::milliseconds{100});
        return;
    }
    startWork(listener_, listenerWork_, [this] {
        return QtConcurrent::run(checkKernelListener, core_->port(), core_->processId());
    });
}

void ServiceHost::sendHttp(const QByteArray& path, const QByteArray& method,
                           const QByteArray& body) {
    QNetworkRequest request(
        QUrl::fromEncoded("http://127.0.0.1:" + QByteArray::number(core_->port()) + path));
    request.setRawHeader("Authorization", "Bearer " + core_->secret().toUtf8());
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(
        coreRequestTimeout(path, method, static_cast<int>(timeout_budget::request.count())));
    auto* reply =
        method == "GET" ? network_.get(request) : network_.sendCustomRequest(request, method, body);
    reply_ = reply;
    reply->setReadBufferSize((qint64{8} * 1024 * 1024) + 1);
    connect(reply, &QNetworkReply::readyRead, this, [reply] {
        if (reply->bytesAvailable() > qint64{8} * 1024 * 1024) {
            reply->abort();
        }
    });
    const auto generation = generation_;
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, generation] { httpFinished(reply, generation); });
}

void ServiceHost::httpFinished(QNetworkReply* reply, quint64 generation) {
    reply->deleteLater();
    if (generation != generation_ || stopping_) {
        return;
    }
    reply_.clear();
    const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const auto body = reply->readAll();
    if (operation_ == Operation::EnvironmentDns) {
        const bool success = reply->error() == QNetworkReply::NoError && status == 204;
        finish(200);
        if (success) {
            ++dnsFlushes_;
            reportEnvironment(QStringLiteral("已请求内核清理 DNS 缓存；网络连通性尚未验证"));
        } else {
            reportEnvironment(QStringLiteral("DNS 缓存刷新失败：%1").arg(reply->errorString()));
            dnsPending_ = dnsAttempts_ < 3;
        }
        environmentRetry_.start(success ? 0 : 1000);
        return;
    }
    if (reply->error() != QNetworkReply::NoError || (status != 200 && status != 204)) {
        if (operation_ == Operation::Starting) {
            poll_.start(std::chrono::milliseconds{100});
        } else if (operation_ == Operation::Api || operation_ == Operation::Mode ||
                   operation_ == Operation::ConfigApply || operation_ == Operation::ConfigGeneral) {
            finish(status == 0 ? 503 : status, body, reply->errorString());
        } else {
            stop(activeId_, reply->errorString());
        }
        return;
    }
    if (operation_ == Operation::Starting) {
        version_ =
            QJsonDocument::fromJson(body).object().value(QStringLiteral("version")).toString();
        if (version_.isEmpty() || version_.size() > 128) {
            stop(activeId_, QStringLiteral("Invalid core version."));
            return;
        }
        operation_ = Operation::VerifyVersion;
        readVersion();
    } else if (operation_ == Operation::InitialGeneral) {
        const auto port =
            QJsonDocument::fromJson(body).object().value(QStringLiteral("mixed-port")).toInteger();
        if (port < 1 || port > 65535) {
            stop(activeId_, QStringLiteral("Invalid core mixed port."));
            return;
        }
        mixedPort_ = static_cast<quint16>(port);
        const auto config =
            compileServiceConfig(QStringLiteral(R"({"mode":"rule"})"), mixedPort_, {});
        if (!config.error.isEmpty()) {
            stop(activeId_, config.error);
            return;
        }
        pendingConfig_ = config.value;
        operation_ = Operation::InitialConfig;
        sendHttp(
            "/configs?force=true", "PUT",
            QJsonDocument(QJsonObject{
                              {
                                  QStringLiteral("payload"),
                                  QString::fromUtf8(
                                      QJsonDocument(config.value).toJson(QJsonDocument::Compact)),
                              },
                          })
                .toJson(QJsonDocument::Compact));
    } else if (operation_ == Operation::InitialConfig) {
        runtimeConfig_ = pendingConfig_;
        openTraffic();
        reportEnvironment(QStringLiteral("后台服务网络监测已就绪"));
        finish(200, information());
    } else if (operation_ == Operation::Enable) {
        runtimeConfig_ = pendingConfig_;
        operation_ = Operation::VerifyTun;
        verifyTun();
    } else if (operation_ == Operation::Disable) {
        auto tun = runtimeConfig_.value(QStringLiteral("tun")).toObject();
        tun.insert(QStringLiteral("enable"), false);
        runtimeConfig_.insert(QStringLiteral("tun"), tun);
        cleanup(Cleanup::TunOff);
    } else if (operation_ == Operation::ConfigApply) {
        runtimeConfig_ = pendingConfig_;
        operation_ = Operation::ConfigGeneral;
        deadline_.start(timeout_budget::service_verification);
        sendHttp("/configs");
    } else if (operation_ == Operation::Mode) {
        runtimeConfig_ = pendingConfig_;
        finish(204);
    } else if (operation_ == Operation::ConfigGeneral) {
        const auto actual = QJsonDocument::fromJson(body).object();
        const auto port = actual.value(QStringLiteral("mixed-port")).toInteger();
        if (port < 1 || port > 65535) {
            finish(503, "{}", QStringLiteral("The configured mixed listener is not available."));
            return;
        }
        mixedPort_ = static_cast<quint16>(port);
        if (tunActive_) {
            operation_ = Operation::VerifyConfigTun;
            verifyTun();
        } else {
            finish(204);
        }
    } else {
        finish(status, body);
    }
}

void ServiceHost::readConnections(const QString& id) {
    if (monitor_) {
        respond(id, 409, "{}", QStringLiteral("A connection snapshot is already pending."));
        return;
    }
    QNetworkRequest request(
        QUrl(QStringLiteral("http://127.0.0.1:%1/connections").arg(core_->port())));
    request.setRawHeader("Authorization", "Bearer " + core_->secret().toUtf8());
    request.setTransferTimeout(5000);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    auto* reply = network_.get(request);
    monitor_ = reply;
    reply->setReadBufferSize((qint64{8} * 1024 * 1024) + 1);
    connect(reply, &QNetworkReply::readyRead, this, [reply] {
        if (reply->bytesAvailable() > qint64{8} * 1024 * 1024) {
            reply->abort();
        }
    });
    const auto generation = generation_;
    connect(reply, &QNetworkReply::finished, this, [this, reply, id, generation] {
        reply->deleteLater();
        if (generation != generation_ || stopping_) {
            return;
        }
        monitor_.clear();
        const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto body = reply->readAll();
        if (body.size() > qint64{8} * 1024 * 1024) {
            respond(id, 413, "{}", QStringLiteral("Connection snapshot is too large."));
            return;
        }
        respond(id, status == 0 ? 503 : status, body,
                reply->error() == QNetworkReply::NoError ? QString{} : reply->errorString());
    });
}

void ServiceHost::forwardApi(const QJsonObject& arguments) {
    if (arguments.size() != 3 || !arguments.value(QStringLiteral("path")).isString() ||
        !arguments.value(QStringLiteral("method")).isString() ||
        !arguments.value(QStringLiteral("body")).isString()) {
        finish(400, "{}", QStringLiteral("Malformed API request."));
        return;
    }
    const auto path = arguments.value(QStringLiteral("path")).toString().toLatin1();
    const auto method = arguments.value(QStringLiteral("method")).toString().toLatin1();
    const auto decoded = QByteArray::fromBase64Encoding(
        arguments.value(QStringLiteral("body")).toString().toLatin1(),
        QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded || decoded.decoded.size() > qint64{8} * 1024 * 1024) {
        finish(400, "{}", QStringLiteral("Invalid API body encoding or size."));
        return;
    }
    const auto& body = decoded.decoded;
    const auto payload = QJsonDocument::fromJson(body).object();
    operation_ = Operation::Api;
    if (method == "POST" && path == "/configs/geo" && body.isEmpty()) {
        deadline_.start(timeout_budget::geo_update + timeout_budget::service_overhead);
        sendHttp(path, method);
        return;
    }
    if (method == "GET" && body.isEmpty() &&
        (path == "/version" || path == "/configs" || path == "/proxies" || path == "/connections" ||
         path == "/rules")) {
        sendHttp(path);
        return;
    }
    if (method == "PATCH" && path == "/configs" && payload.size() == 1) {
        const auto mode = payload.value(QStringLiteral("mode")).toString();
        if (mode == QStringLiteral("rule") || mode == QStringLiteral("global") ||
            mode == QStringLiteral("direct")) {
            pendingConfig_ = runtimeConfig_;
            pendingConfig_.insert(QStringLiteral("mode"), mode);
            operation_ = Operation::Mode;
            sendHttp(path, method, QJsonDocument(payload).toJson(QJsonDocument::Compact));
            return;
        }
    }
    if (((method == "DELETE" && path == "/connections") ||
         (method == "POST" && path == "/cache/dns/flush")) &&
        body.isEmpty()) {
        sendHttp(path, method);
        return;
    }
    if (method == "DELETE" && body.isEmpty() && path.startsWith("/connections/")) {
        const auto id = QString::fromLatin1(path.sliced(13));
        const QUuid uuid(id);
        if (!uuid.isNull() && uuid.toString(QUuid::WithoutBraces) == id) {
            sendHttp(path, method);
            return;
        }
    }
    if (method == "PUT" && path == "/configs?force=true" && payload.size() == 1) {
        deadline_.start(
            coreRequestTimeout(path, method, static_cast<int>(timeout_budget::request.count())) +
            static_cast<int>(timeout_budget::service_overhead.count()));
        if (compilerWork_) {
            finish(409, "{}",
                   QStringLiteral("Previous configuration compiler is still finishing."));
            return;
        }
        const auto source = payload.value(QStringLiteral("payload"));
        if (source.isString() && source.toString().size() <= qint64{4} * 1024 * 1024 &&
            !source.toString().isEmpty()) {
            const auto cacheError = core_->providerCacheError();
            if (!cacheError.isEmpty()) {
                finish(403, "{}", cacheError);
                return;
            }
            operation_ = Operation::Compile;
            startWork(compiler_, compilerWork_, [this, &source] {
                return QtConcurrent::run(compileServiceConfig, source.toString(), mixedPort_,
                                         plan_.device, tunActive_, core_->workingDirectory());
            });
            return;
        }
    }
    const auto resource = serviceResourceEndpoint(path, method, body);
    if (!resource.isEmpty()) {
        if (resource.startsWith("/providers/")) {
            const auto error = core_->providerCacheError();
            if (!error.isEmpty()) {
                finish(403, "{}", error);
                return;
            }
        }
        deadline_.start(coreRequestTimeout(resource, method,
                                           static_cast<int>(timeout_budget::request.count())) +
                        static_cast<int>(timeout_budget::service_overhead.count()));
        sendHttp(resource, method, body);
        return;
    }
    const auto endpoint = serviceProxyEndpoint(path, method, body);
    if (!endpoint.isEmpty()) {
        sendHttp(endpoint, method, body);
        return;
    }
    finish(403, "{}", QStringLiteral("This core API operation is not admitted by the service."));
}

ServiceHost::WorkToken ServiceHost::workToken() const noexcept {
    return {
        .kernelEpoch = generation_,
        .tunEpoch = tunGeneration_,
        .operation = operation_,
        .guid = plan_.guid,
    };
}

void ServiceHost::enableTun() {
    operation_ = Operation::Prepare;
    ++tunGeneration_;
    healthFailure_.invalidate();
    startWork(planner_, plannerWork_, [this] {
        return QtConcurrent::run(prepareTun, journal_, requirementsFor(runtimeConfig_));
    });
}

void ServiceHost::verifyTun() {
    if (!observerWork_) {
        startWork(observer_, observerWork_, [this] {
            return QtConcurrent::run(
                observeTun, journal_,
                std::optional<TunRequirements>(requirementsFor(runtimeConfig_)));
        });
    }
}

void ServiceHost::stop(const QString& id, const QString& error) {
    trafficRequested_ = false;
    logLevel_.clear();
    if (stopping_) {
        if (!id.isEmpty()) {
            respond(id, 409, "{}", QStringLiteral("Cleanup is already in progress."));
        }
        return;
    }
    stopping_ = true;
    logs_.stop();
    logLevel_.clear();
    repair_ = Repair::Exhausted;
    refreshPending_ = false;
    dnsPending_ = false;
    healthPending_ = false;
    environmentRetry_.stop();
    trafficWatchdog_.stop();
    reportEnvironment(error.isEmpty() ? QStringLiteral("正在停止并清理网络资源") : error);
    stopError_ = error;
    if (!activeId_.isEmpty() && activeId_ != id) {
        respond(activeId_, 503, "{}", QStringLiteral("Operation canceled for cleanup."));
    }
    activeId_ = id;
    operation_ = Operation::Stop;
    ++generation_;
    deadline_.stop();
    if (monitor_) {
        QObject::disconnect(monitor_, nullptr, this, nullptr);
        monitor_->abort();
        monitor_->deleteLater();
        monitor_.clear();
    }
    poll_.stop();
    trafficRetry_.stop();
    traffic_.abort();
    processEvent_.reset();
    if (reply_) {
        reply_->abort();
        reply_.clear();
    }
    stopProcess();
}

void ServiceHost::stopProcess() {
    // worker 的结果尚未被主线程接收前，仍可能持有目录或写入 TUN 日志。
    if (launchWork_ || compilerWork_ || plannerWork_ || observerWork_ || stopWork_ ||
        cleanupWork_) {
        return;
    }
    if (!core_) {
        cleanup(Cleanup::Stop);
        return;
    }
    const auto core = core_;
    const auto helper = options_.signalHelper;
    startWork(stopper_, stopWork_,
              [&] { return QtConcurrent::run([core, helper] { return core->stop(helper); }); });
}

void ServiceHost::cleanup(Cleanup purpose) {
    cleanup_ = purpose;
    operation_ = Operation::Cleanup;
    startWork(cleaner_, cleanupWork_, [] {
        return QtConcurrent::run(runOwnedTool, QCoreApplication::applicationFilePath(),
                                 QStringList{QStringLiteral("--cleanup-tun")},
                                 serviceDataDirectory(), 18000);
    });
}

void ServiceHost::cleanupFinished() {
    cleanupWork_.reset();
    const auto error = cleaner_.future().takeResult();
    if (stopping_ && cleanup_ != Cleanup::Stop) {
        if (!error.isEmpty()) {
            stopError_ = error;
        }
        stopProcess();
        return;
    }
    if (!error.isEmpty()) {
        if (cleanup_ == Cleanup::TunOff) {
            stop(activeId_, error);
            return;
        }
        blocked_ = true;
        finish(500, "{}", error);
        Q_EMIT fatal(error);
        return;
    }
    plan_ = {};
    tunActive_ = false;
    if (cleanup_ == Cleanup::Startup) {
        operation_ = Operation::Idle;
        try {
            pipe_.listen();
            Q_EMIT ready();
        } catch (const std::exception& failure) {
            Q_EMIT fatal(QString::fromUtf8(failure.what()));
        }
        return;
    }
    if (cleanup_ == Cleanup::Stop) {
        // Job 退出和所属 TUN 清理均确认后，才能向客户端宣告资源已释放。
        stopping_ = false;
        reportEnvironment(stopError_.isEmpty() ? QStringLiteral("网络资源已清理") : stopError_);
        pipe_.send({
            {QStringLiteral("event"), QStringLiteral("stopped")},
            {QStringLiteral("error"), stopError_},
        });
        finish(stopError_.isEmpty() ? 200 : 500, "{}", stopError_);
        if (serviceStopping_) {
            Q_EMIT drained();
        } else {
            pipe_.resume();
        }
        return;
    }
    if (repair_ == Repair::InProgress) {
        deadline_.start(timeout_budget::tun);
        enableTun();
        return;
    }
    finish(200, "{\"active\":false}");
}

QByteArray ServiceHost::information() const {
    return QJsonDocument(
               QJsonObject{
                   {
                       QStringLiteral("operation"),
                       QString::fromLatin1(QMetaEnum::fromType<Operation>().valueToKey(
                           static_cast<int>(operation_))),
                   },
                   {
                       QStringLiteral("cleanup-purpose"),
                       QString::fromLatin1(
                           QMetaEnum::fromType<Cleanup>().valueToKey(static_cast<int>(cleanup_))),
                   },
                   {
                       QStringLiteral("repair"),
                       QString::fromLatin1(
                           QMetaEnum::fromType<Repair>().valueToKey(static_cast<int>(repair_))),
                   },
                   {QStringLiteral("blocked"), blocked_},
                   {QStringLiteral("stopping"), stopping_},
                   {QStringLiteral("shutting-down"), serviceStopping_},
                   {QStringLiteral("generation"), QString::number(generation_)},
                   {QStringLiteral("tun-generation"), QString::number(tunGeneration_)},
                   {
                       QStringLiteral("pending-workers"),
                       QJsonObject{
                           {QStringLiteral("launch"), launchWork_.has_value()},
                           {QStringLiteral("compile"), compilerWork_.has_value()},
                           {QStringLiteral("plan"), plannerWork_.has_value()},
                           {QStringLiteral("observe"), observerWork_.has_value()},
                           {QStringLiteral("stop"), stopWork_.has_value()},
                           {QStringLiteral("cleanup"), cleanupWork_.has_value()},
                           {QStringLiteral("health"), healthWork_.has_value()},
                       },
                   },
                   {QStringLiteral("request-id"), activeId_},
                   {QStringLiteral("pid"), core_ ? static_cast<qint64>(core_->processId()) : 0},
                   {QStringLiteral("version"), version_},
                   {QStringLiteral("active"), tunActive_},
                   {QStringLiteral("tun-device"), plan_.device},
                   {QStringLiteral("tun-guid"), plan_.guid},
                   {QStringLiteral("tun-luid"), static_cast<qint64>(plan_.luid)},
                   {QStringLiteral("maintenance"), repair_ == Repair::InProgress},
                   {QStringLiteral("environment"), environmentMessage_},
                   {QStringLiteral("environment-checks"), qint64{environmentChecks_}},
                   {QStringLiteral("dns-flushes"), qint64{dnsFlushes_}},
                   {QStringLiteral("tun-repairs"), qint64{tunRepairs_}},
               })
        .toJson(QJsonDocument::Compact);
}

void ServiceHost::respond(const QString& id, int status, const QByteArray& body,
                          const QString& error) {
    if (!id.isEmpty()) {
        pipe_.send({
            {QStringLiteral("id"), id},
            {QStringLiteral("status"), status},
            {QStringLiteral("body"), QString::fromLatin1(body.toBase64())},
            {QStringLiteral("error"), error.left(512)},
        });
    }
}

void ServiceHost::finish(int status, const QByteArray& body, const QString& error) {
    deadline_.stop();
    const auto id = std::exchange(activeId_, {});
    operation_ = Operation::Idle;
    respond(id, status, body, error);
}

void ServiceHost::openLogs() {
    if (!core_ || stopping_ || suspended_ || logLevel_.isEmpty()) {
        return;
    }
    QNetworkRequest request(
        QUrl(QStringLiteral("ws://127.0.0.1:%1/logs?level=%2").arg(core_->port()).arg(logLevel_)));
    request.setRawHeader("Authorization", "Bearer " + core_->secret().toUtf8());
    logs_.start(request);
}

void ServiceHost::openTraffic() {
    if (!trafficRequested_ || !core_ || stopping_ || suspended_) {
        return;
    }
    traffic_.abort();
    trafficRetry_.stop();
    trafficWatchdog_.start(std::chrono::milliseconds{5000});
    QNetworkRequest request(QUrl(QStringLiteral("ws://127.0.0.1:%1/traffic").arg(core_->port())));
    request.setRawHeader("Authorization", "Bearer " + core_->secret().toUtf8());
    traffic_.open(request);
}

void ServiceHost::reportEnvironment(const QString& message) {
    environmentMessage_ = message.left(1024);
    pipe_.send({
        {QStringLiteral("event"), QStringLiteral("environment")},
        {QStringLiteral("message"), environmentMessage_},
        {
            QStringLiteral("repairing"),
            repair_ == Repair::InProgress || operation_ == Operation::EnvironmentDns,
        },
    });
}

void ServiceHost::environmentObserved(const NetworkSnapshot& snapshot, EnvironmentBatch batch,
                                      bool changed) {
    if (batch.kinds != 0) {
        ++environmentChecks_;
    }
    const bool resumed = batch.resumed() || (suspended_ && !batch.suspended);
    suspended_ = batch.suspended;
    online_ = snapshot.online;
    if (suspended_) {
        logs_.stop();
        trafficWatchdog_.stop();
        trafficRetry_.stop();
        traffic_.abort();
        environmentRetry_.stop();
        reportEnvironment(QStringLiteral("系统挂起，保留配置并等待恢复"));
        return;
    }
    if (!core_ || stopping_ || serviceStopping_) {
        return;
    }
    if (changed || resumed) {
        openLogs();
        refreshPending_ = true;
        dnsPending_ = true;
        dnsAttempts_ = 0;
        healthAttempts_ = 0;
        if (repair_ != Repair::InProgress) {
            repair_ = Repair::Available;
            healthFailure_.invalidate();
        }
    }
    if (tunActive_ && batch.kinds != 0) {
        healthPending_ = true;
        healthAttempts_ = 0;
    }
    if (refreshPending_ || healthPending_ || (online_ && dnsPending_)) {
        environmentRetry_.start(std::chrono::milliseconds{0});
    }
}

void ServiceHost::checkEnvironment() {
    if (!core_ || stopping_ || serviceStopping_ || suspended_) {
        return;
    }
    if (operation_ != Operation::Idle || healthWork_) {
        environmentRetry_.start(std::chrono::milliseconds{500});
        return;
    }
    if (refreshPending_) {
        refreshPending_ = false;
        network_.clearConnectionCache();
        if (traffic_.state() == QAbstractSocket::UnconnectedState && !trafficRetry_.isActive()) {
            openTraffic();
        }
    }
    if (dnsPending_ && online_) {
        dnsPending_ = false;
        ++dnsAttempts_;
        operation_ = Operation::EnvironmentDns;
        deadline_.start(std::chrono::milliseconds{10000});
        reportEnvironment(QStringLiteral("网络变化，正在刷新内核 DNS 缓存"));
        sendHttp("/cache/dns/flush", "POST");
        return;
    }
    if (healthPending_ && tunActive_) {
        healthPending_ = false;
        startWork(health_, healthWork_, [this] { return QtConcurrent::run(inspectTun, journal_); });
    } else if (!online_) {
        reportEnvironment(QStringLiteral("等待物理网络恢复，保留 TUN 与代理配置"));
    }
}

void ServiceHost::environmentHealthFinished() {
    const auto token = std::exchange(healthWork_, std::nullopt);
    const auto result = health_.future().takeResult();
    if (!token || !core_ || stopping_ || serviceStopping_ || token->kernelEpoch != generation_ ||
        !tunActive_ || token->tunEpoch != tunGeneration_ || plan_.guid != token->guid) {
        return;
    }
    if (operation_ != Operation::Idle) {
        healthPending_ = true;
        environmentRetry_.start(std::chrono::milliseconds{500});
        return;
    }
    if (result.healthy) {
        healthFailure_.invalidate();
        reportEnvironment(online_ ? QStringLiteral("TUN 网卡与配置要求的路由已核验")
                                  : QStringLiteral("TUN 正常，等待物理网络恢复"));
        return;
    }
    if (!result.repairable) {
        reportEnvironment(QStringLiteral("无法确定 TUN 现场，未重置：%1").arg(result.error));
        if (++healthAttempts_ < 3) {
            healthPending_ = true;
            environmentRetry_.start(std::chrono::milliseconds{1000});
        }
        return;
    }
    if (!online_) {
        reportEnvironment(QStringLiteral("等待物理网络稳定后复查 TUN"));
        return;
    }
    if (!healthFailure_.isValid()) {
        healthFailure_.start();
    }
    if (healthFailure_.elapsed() < 5000) {
        healthPending_ = true;
        environmentRetry_.start(std::chrono::milliseconds{1000});
        return;
    }
    if (repair_ != Repair::Available) {
        stop({}, QStringLiteral("TUN 修复后仍异常，已停止内核并清理，避免反复接管"));
        return;
    }
    repair_ = Repair::InProgress;
    ++tunGeneration_;
    operation_ = Operation::Disable;
    deadline_.start(timeout_budget::tun);
    reportEnvironment(QStringLiteral("已确认 TUN 设备或默认路由缺失，正在重建所属 TUN"));
    sendHttp("/configs", "PATCH", R"({"tun":{"enable":false}})");
}

void ServiceHost::shutdown() {
    serviceStopping_ = true;
    stop();
}
