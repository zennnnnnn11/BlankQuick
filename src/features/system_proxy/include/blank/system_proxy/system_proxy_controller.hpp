#ifndef BLANK_QUICK_SYSTEM_PROXY_CONTROLLER_HPP
#define BLANK_QUICK_SYSTEM_PROXY_CONTROLLER_HPP

#include <QProcess>
#include <QTimer>
#include <chrono>

struct SystemProxyOptions {
    QString helper;
    QString journal;
    std::chrono::milliseconds operationTimeout{15000};
    std::chrono::milliseconds restoreTimeout{15000};
};

struct SystemProxyContext {
    quint32 processId = 0;
    quint16 mixedPort = 0;
    bool ready = false;
    bool busy = true;
    [[nodiscard]] bool operator==(const SystemProxyContext&) const = default;
};

class SystemProxyController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool active READ active NOTIFY changed FINAL)
    Q_PROPERTY(bool busy READ busy NOTIFY changed FINAL)
    Q_PROPERTY(bool available READ available NOTIFY changed FINAL)
    Q_PROPERTY(QString message READ message NOTIFY changed FINAL)

  public:
    enum class LeaseState : quint8 {
        Released,
        ForeignOwner,
        Owned,
        Pending,
        Uncertain,
    };
    enum class State : quint8 {
        Off,
        Foreign,
        Recovering,
        Applying,
        On,
        Restoring,
        RecoveryRequired,
    };
    enum class Failure : quint8 {
        None,
        Start,
        Timeout,
        Protocol,
        Helper,
        Conflict,
    };
    explicit SystemProxyController(SystemProxyOptions options, QObject* parent = nullptr);
    ~SystemProxyController() override = default;
    SystemProxyController(const SystemProxyController&) = delete;
    SystemProxyController& operator=(const SystemProxyController&) = delete;
    SystemProxyController(SystemProxyController&&) = delete;
    SystemProxyController& operator=(SystemProxyController&&) = delete;
    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] LeaseState leaseState() const noexcept;
    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] bool available() const noexcept;
    [[nodiscard]] State state() const noexcept;
    [[nodiscard]] Failure failure() const noexcept;
    [[nodiscard]] bool recoverable() const noexcept;
    [[nodiscard]] bool helperRunning() const noexcept;
    [[nodiscard]] const QString& message() const noexcept;
    void updateContext(SystemProxyContext context);
    void recover();
    void checkEnvironment();
    Q_SLOT void setEnabled(bool enabled);

  Q_SIGNALS:
    void changed();

  private:
    enum class Claim : quint8 {
        Released,
        Owned,
        Uncertain,
    };
    void launch(const QStringList& arguments, State nextState);
    void readOutput();
    void finished();
    void restore();
    void timedOut();
    void protocolFailure();
    SystemProxyContext context_;
    SystemProxyOptions options_;
    QProcess worker_;
    QTimer deadline_;
    State state_ = State::Off;
    QByteArray output_;
    QString result_;
    QString message_;
    QString error_;
    Claim claim_ = Claim::Released;
    Failure failure_ = Failure::None;
    bool environmentPending_ = false;
};

#endif
