#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkProxy>
#include <algorithm>
#include <blank/qt/child.hpp>
#include <blank/telemetry/traffic_controller.hpp>
#include <chrono>
#include <utility>

TrafficController::TrafficController(QObject* parent) : QObject(parent) {
    reconnect_.setSingleShot(true);
    watchdog_.setSingleShot(true);
    connect(&reconnect_, &QTimer::timeout, this, &TrafficController::open);
    connect(&watchdog_, &QTimer::timeout, this, [this] { retry(tr("流量数据超时，正在重连")); });
    reset();
}

TrafficController::~TrafficController() {
    discardSocket();
}

const QString& TrafficController::status() const noexcept {
    return status_;
}

bool TrafficController::connected() const noexcept {
    return connected_;
}

quint64 TrafficController::upload() const noexcept {
    return upload_;
}

quint64 TrafficController::download() const noexcept {
    return download_;
}

void TrafficController::discardSocket() {
    // 先换代再断开。abort/disconnected 可能产生同步或已排队回调，旧代次
    // 不能触发新流的重连；deleteLater 只负责对象回收，不承担状态隔离。
    ++generation_;
    if (socket_) {
        auto* previous = socket_.data();
        socket_.clear();
        QObject::disconnect(previous, nullptr, this, nullptr);
        previous->abort();
        previous->deleteLater();
    }
}

void TrafficController::setEnabled(bool enabled) {
    if (enabled_ == enabled) {
        return;
    }
    enabled_ = enabled;
    if (!enabled_ && context_.serviceManaged && context_.ready) {
        Q_EMIT serviceSubscriptionRequested(false, generation_);
    }
    reset();
}

void TrafficController::updateContext(TrafficContext context) {
    if (context_ == context) {
        return;
    }
    context_ = std::move(context);
    reset();
}

void TrafficController::receiveServiceMessage(const QString& message) {
    if (context_.serviceManaged && context_.ready) {
        received(message);
    }
}

void TrafficController::serviceRequestFinished(quint64 generation, bool success) {
    if (enabled_ && context_.serviceManaged && context_.ready && generation == generation_ &&
        !success) {
        retry(tr("服务流量流未就绪，正在重连"));
    }
}

void TrafficController::reset() {
    reconnect_.stop();
    watchdog_.stop();
    discardSocket();
    upload_ = 0;
    download_ = 0;
    connected_ = false;
    retryMs_ = 1000;
    status_ = tr("内核未就绪");
    if (enabled_ && context_.ready) {
        open();
    }
    Q_EMIT changed();
}

void TrafficController::open() {
    // 服务模式只向应用层发出订阅意图，直接模式才建立 WebSocket。
    // 两条路径共用消息校验和看门狗，读取器不拥有也不重启内核。
    if (!enabled_ || suspended_ || !context_.ready || socket_) {
        return;
    }
    if (context_.serviceManaged) {
        status_ = tr("等待服务转发实时流量数据");
        watchdog_.start(std::chrono::milliseconds{5000});
        Q_EMIT serviceSubscriptionRequested(true, generation_);
        Q_EMIT changed();
        return;
    }
    auto* socket =
        &blank::qt::makeChild<QWebSocket>(*this, QString{}, QWebSocketProtocol::VersionLatest);
    socket_ = socket;
    const auto generation = generation_;
    socket->setProxy(QNetworkProxy::NoProxy);
    socket->setReadBufferSize(8192);
    socket->setMaxAllowedIncomingFrameSize(4096);
    socket->setMaxAllowedIncomingMessageSize(4096);
    connect(socket, &QWebSocket::textMessageReceived, this,
            [this, generation](const QString& message) {
                if (generation == generation_) {
                    received(message);
                }
            });
    connect(socket, &QWebSocket::binaryMessageReceived, this,
            [this, generation](const QByteArray&) {
                if (generation == generation_) {
                    retry(tr("流量消息类型无效，正在重连"));
                }
            });
    connect(socket, &QWebSocket::errorOccurred, this,
            [this, socket, generation](QAbstractSocket::SocketError) {
                if (generation == generation_) {
                    retry(tr("流量连接失败，正在重连：%1").arg(socket->errorString().left(300)));
                }
            });
    connect(socket, &QWebSocket::disconnected, this, [this, generation] {
        if (generation == generation_) {
            retry(tr("流量连接断开，正在重连"));
        }
    });
    auto request = context_.request;
    auto url = request.url();
    url.setScheme(QStringLiteral("ws"));
    request.setUrl(url);
    status_ = tr("等待实时流量数据");
    watchdog_.start(std::chrono::milliseconds{5000});
    socket->open(request);
    Q_EMIT changed();
}

void TrafficController::environmentChanged(bool suspended) {
    suspended_ = suspended;
    reconnect_.stop();
    watchdog_.stop();
    discardSocket();
    connected_ = false;
    upload_ = 0;
    download_ = 0;
    retryMs_ = 1000;
    status_ = suspended ? tr("系统即将挂起") : tr("网络环境变化，正在恢复流量流");
    if (!suspended) {
        open();
    }
    Q_EMIT changed();
}

void TrafficController::retry(const QString& message) {
    watchdog_.stop();
    discardSocket();
    upload_ = 0;
    download_ = 0;
    connected_ = false;
    status_ = message;
    if (context_.ready) {
        reconnect_.start(retryMs_);
        retryMs_ = std::min(retryMs_ * 2, 10000);
    }
    Q_EMIT changed();
}

void TrafficController::received(const QString& message) {
    // 成功握手不等于持续有数据。每条合法样本才刷新看门狗；断流时清零展示，
    // 防止界面把过期速率继续显示成实时数据。
    if (!enabled_ || suspended_) {
        return;
    }
    const auto object = QJsonDocument::fromJson(message.toUtf8()).object();
    const auto up = object.value(QStringLiteral("up")).toInteger(-1);
    const auto down = object.value(QStringLiteral("down")).toInteger(-1);
    if (up < 0 || down < 0) {
        retry(tr("流量数据无效，正在重连"));
        return;
    }
    upload_ = static_cast<quint64>(up);
    download_ = static_cast<quint64>(down);
    connected_ = true;
    retryMs_ = 1000;
    status_ = tr("实时");
    watchdog_.start(std::chrono::milliseconds{5000});
    Q_EMIT changed();
}
