#include "traffic_controller.hpp"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <algorithm>
#include <blank/qt/child.hpp>
#include <chrono>

TrafficController::TrafficController(KernelController& kernel, QObject* parent)
    : QObject(parent), kernel_(kernel)
{
    reconnect_.setSingleShot(true);
    watchdog_.setSingleShot(true);
    connect(&reconnect_, &QTimer::timeout, this, &TrafficController::open);
    connect(&watchdog_, &QTimer::timeout, this, [this] { retry(tr("流量数据超时，正在重连")); });
    connect(&kernel_, &KernelController::stateChanged, this, &TrafficController::kernelChanged);
    connect(&kernel_, &KernelController::serviceTraffic, this,
            [this](const QString& message)
            {
                if (kernel_.serviceManaged() && kernel_.state() == KernelController::State::Ready)
                {
                    received(message);
                }
            });
    kernelChanged();
}

TrafficController::~TrafficController()
{
    discardSocket();
}

const QString& TrafficController::status() const noexcept
{
    return status_;
}

bool TrafficController::connected() const noexcept
{
    return connected_;
}

quint64 TrafficController::upload() const noexcept
{
    return upload_;
}

quint64 TrafficController::download() const noexcept
{
    return download_;
}

void TrafficController::discardSocket()
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
}

void TrafficController::setEnabled(bool enabled)
{
    if (enabled_ == enabled)
    {
        return;
    }
    enabled_ = enabled;
    if (!enabled_ && kernel_.serviceManaged() && kernel_.state() == KernelController::State::Ready)
    {
        auto const* reply = kernel_.serviceRequest(QStringLiteral("traffic-stop"));
        connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
    }
    kernelChanged();
}

void TrafficController::kernelChanged()
{
    reconnect_.stop();
    watchdog_.stop();
    discardSocket();
    upload_ = 0;
    download_ = 0;
    connected_ = false;
    retryMs_ = 1000;
    status_ = tr("内核未就绪");
    if (enabled_ && kernel_.state() == KernelController::State::Ready)
    {
        open();
    }
    Q_EMIT changed();
}

void TrafficController::open()
{
    if (!enabled_ || suspended_ || kernel_.state() != KernelController::State::Ready || socket_)
    {
        return;
    }
    if (kernel_.serviceManaged())
    {
        status_ = tr("等待服务转发实时流量数据");
        watchdog_.start(std::chrono::milliseconds{5000});
        const auto generation = generation_;
        auto* reply = kernel_.serviceRequest(QStringLiteral("traffic"));
        connect(reply, &QNetworkReply::finished, this,
                [this, reply, generation]
                {
                    reply->deleteLater();
                    if (generation == generation_ && reply->error() != QNetworkReply::NoError)
                    {
                        retry(tr("服务流量流未就绪，正在重连"));
                    }
                });
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
            [this, generation](const QString& message)
            {
                if (generation == generation_)
                {
                    received(message);
                }
            });
    connect(socket, &QWebSocket::binaryMessageReceived, this,
            [this, generation](const QByteArray&)
            {
                if (generation == generation_)
                {
                    retry(tr("流量消息类型无效，正在重连"));
                }
            });
    connect(socket, &QWebSocket::errorOccurred, this,
            [this, socket, generation](QAbstractSocket::SocketError)
            {
                if (generation == generation_)
                {
                    retry(tr("流量连接失败，正在重连：%1").arg(socket->errorString().left(300)));
                }
            });
    connect(socket, &QWebSocket::disconnected, this,
            [this, generation]
            {
                if (generation == generation_)
                {
                    retry(tr("流量连接断开，正在重连"));
                }
            });
    auto request = kernel_.apiRequest("/traffic");
    auto url = request.url();
    url.setScheme(QStringLiteral("ws"));
    request.setUrl(url);
    status_ = tr("等待实时流量数据");
    watchdog_.start(std::chrono::milliseconds{5000});
    socket->open(request);
    Q_EMIT changed();
}

void TrafficController::environmentChanged(bool suspended)
{
    suspended_ = suspended;
    reconnect_.stop();
    watchdog_.stop();
    discardSocket();
    connected_ = false;
    upload_ = 0;
    download_ = 0;
    retryMs_ = 1000;
    status_ = suspended ? tr("系统即将挂起") : tr("网络环境变化，正在恢复流量流");
    if (!suspended)
    {
        open();
    }
    Q_EMIT changed();
}

void TrafficController::retry(const QString& message)
{
    watchdog_.stop();
    discardSocket();
    upload_ = 0;
    download_ = 0;
    connected_ = false;
    status_ = message;
    if (kernel_.state() == KernelController::State::Ready)
    {
        reconnect_.start(retryMs_);
        retryMs_ = std::min(retryMs_ * 2, 10000);
    }
    Q_EMIT changed();
}

void TrafficController::received(const QString& message)
{
    if (!enabled_ || suspended_)
    {
        return;
    }
    const auto object = QJsonDocument::fromJson(message.toUtf8()).object();
    const auto up = object.value(QStringLiteral("up")).toInteger(-1);
    const auto down = object.value(QStringLiteral("down")).toInteger(-1);
    if (up < 0 || down < 0)
    {
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
