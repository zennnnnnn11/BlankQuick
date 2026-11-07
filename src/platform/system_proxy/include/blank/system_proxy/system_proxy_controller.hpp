#ifndef BLANK_QUICK_SYSTEM_PROXY_CONTROLLER_HPP
#define BLANK_QUICK_SYSTEM_PROXY_CONTROLLER_HPP

#include <QTimer>

#include <QProcess>
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

// 管理辅助进程与代理租约，不自行读取内核控制器。会话提供 PID/端口/就绪事实，
// 本类报告接管和恢复进度；租约尚未确认释放时，上层必须保持内核存活。
class SystemProxyController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool active READ active NOTIFY changed FINAL)
    Q_PROPERTY(bool busy READ busy NOTIFY changed FINAL)
    Q_PROPERTY(bool available READ available NOTIFY changed FINAL)
    Q_PROPERTY(QString message READ message NOTIFY changed FINAL)

  public:
    // ForeignOwner 表示其他实例持有互斥权，本实例没有接管；Uncertain 则表示
    // 我们可能已经写过系统代理，仍欠一次可确认的恢复，两者不能合并成“关闭”。
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
    ~SystemProxyController() override;
    SystemProxyController(const SystemProxyController&) = delete;
    SystemProxyController& operator=(const SystemProxyController&) = delete;
    SystemProxyController(SystemProxyController&&) = delete;
    SystemProxyController& operator=(SystemProxyController&&) = delete;
    [[nodiscard]] [[maybe_unused]] bool active() const noexcept;
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
    State state_ = State::Off;
    QByteArray output_;
    QString result_;
    QString message_;
    QString error_;
    quint64 generation_ = 0;
    Claim claim_ = Claim::Released;
    Failure failure_ = Failure::None;
    bool environmentPending_ = false;
    QProcess worker_;
    QTimer deadline_;
};

#endif
