#ifndef BLANK_QUICK_SERVICE_CLIENT_HPP
#define BLANK_QUICK_SERVICE_CLIENT_HPP

#include <QTimer>

#include <QHash>
#include <QJsonObject>
#include <QLocalSocket>
#include <QMap>
#include <QNetworkReply>
#include <QPointer>
#include <blank/protocol/json_frame.hpp>

class PipeReply final : public QNetworkReply {
    Q_OBJECT
  public:
    explicit PipeReply(QObject* parent);
    void abort() override;
    void setTimeout(int milliseconds);
    [[nodiscard]] qint64 bytesAvailable() const override;
    void complete(int status, QByteArray body, const QString& failure = {});

  protected:
    qint64 readData(char* data, qint64 maximum) override;

  private:
    QByteArray body_;
    qint64 offset_ = 0;
    QTimer deadline_;
};

class ServiceClient final : public QObject {
    Q_OBJECT
  public:
    explicit ServiceClient(QObject* parent = nullptr);
    ~ServiceClient() override;
    ServiceClient(const ServiceClient&) = delete;
    ServiceClient& operator=(const ServiceClient&) = delete;
    ServiceClient(ServiceClient&&) = delete;
    ServiceClient& operator=(ServiceClient&&) = delete;
    [[nodiscard]] PipeReply* request(const QString& operation, const QJsonObject& arguments = {});
  Q_SIGNALS:
    void traffic(const QString& message);
    void logs(const QString& message);
    void logState(bool connected, const QString& message);
    void lost(const QString& message);
    void coreStopped(const QString& message);
    void environment(const QString& message, bool repairing);

  private:
    void connectPipe();
    void connected();
    void received();
    void fail(const QString& message);
    QLocalSocket socket_;
    blank::protocol::JsonFrames input_;
    QHash<QString, QPointer<PipeReply>> pending_;
    QMap<qint64, QByteArray> unsent_;
    qint64 nextId_ = 0;
    bool verified_ = false;
};

#endif
