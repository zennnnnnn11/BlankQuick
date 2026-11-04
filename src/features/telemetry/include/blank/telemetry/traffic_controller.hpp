#ifndef BLANK_QUICK_TRAFFIC_CONTROLLER_HPP
#define BLANK_QUICK_TRAFFIC_CONTROLLER_HPP

#include <QPointer>
#include <QTimer>
#include <QWebSocket>
#include <blank/runtime/kernel_controller.hpp>

class TrafficController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString status READ status NOTIFY changed FINAL)
    Q_PROPERTY(bool connected READ connected NOTIFY changed FINAL)

  public:
    explicit TrafficController(KernelController& kernel, QObject* parent = nullptr);
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

  Q_SIGNALS:
    void changed();

  private:
    void kernelChanged();
    void open();
    void discardSocket();
    void retry(const QString& message);
    void received(const QString& message);
    KernelController& kernel_;
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
