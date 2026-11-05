#ifndef BLANK_QUICK_TRAFFIC_CONTROLLER_HPP
#define BLANK_QUICK_TRAFFIC_CONTROLLER_HPP

#include <QTimer>

#include <QNetworkRequest>
#include <QPointer>
#include <QWebSocket>

// A telemetry reader receives connection facts, never a process/controller.
// Only RuntimeSession may turn subscription intent into a service command.
struct TrafficContext {
    QNetworkRequest request;
    bool ready = false;
    bool serviceManaged = false;
    [[nodiscard]] bool operator==(const TrafficContext&) const = default;
};

class TrafficController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString status READ status NOTIFY changed FINAL)
    Q_PROPERTY(bool connected READ connected NOTIFY changed FINAL)

  public:
    explicit TrafficController(QObject* parent = nullptr);
    ~TrafficController() override;
    TrafficController(const TrafficController&) = delete;
    TrafficController& operator=(const TrafficController&) = delete;
    TrafficController(TrafficController&&) = delete;
    TrafficController& operator=(TrafficController&&) = delete;
    [[nodiscard]] const QString& status() const noexcept;
    [[nodiscard]] bool connected() const noexcept;
    [[nodiscard]] quint64 upload() const noexcept;
    [[nodiscard]] quint64 download() const noexcept;
    void environmentChanged(bool suspended);
    void setEnabled(bool enabled);
    void updateContext(TrafficContext context);
    void receiveServiceMessage(const QString& message);
    void serviceRequestFinished(quint64 generation, bool success);

  Q_SIGNALS:
    void changed();
    void serviceSubscriptionRequested(bool enabled, quint64 generation);

  private:
    void reset();
    void open();
    void discardSocket();
    void retry(const QString& message);
    void received(const QString& message);
    TrafficContext context_;
    QPointer<QWebSocket> socket_;
    QTimer reconnect_;
    QTimer watchdog_;
    quint64 generation_ = 0;
    quint64 upload_ = 0;
    quint64 download_ = 0;
    int retryMs_ = 1000;
    bool enabled_ = false;
    bool connected_ = false;
    bool suspended_ = false;
    QString status_;
};

#endif
