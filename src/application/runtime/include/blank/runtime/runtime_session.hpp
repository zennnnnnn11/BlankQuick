#ifndef BLANK_RUNTIME_SESSION_HPP
#define BLANK_RUNTIME_SESSION_HPP

#include <QFutureWatcher>
#include <QNetworkAccessManager>
#include <QPointer>
#include <blank/environment/environment_monitor.hpp>
#include <blank/runtime/config_switcher.hpp>
#include <blank/runtime/configuration_preparation.hpp>
#include <blank/runtime/runtime_contract.hpp>
#include <blank/runtime_config/runtime_config.hpp>
#include <blank/streams/core_log_socket.hpp>
#include <blank/system_proxy/system_proxy_controller.hpp>
#include <blank/telemetry/traffic_controller.hpp>
#include <memory>
#include <stop_token>

enum class RuntimeOperation : quint8 {
    Idle,
    Preparing,
    Switching,
    Mode,
    Selection,
    Tun,
    Request,
    SystemProxy,
    Healing,
    Stopping,
};

enum class ConfigurationCertainty : quint8 {
    NoConfiguration,
    Confirmed,
    Uncertain,
};

enum class RuntimeLifecycle : quint8 {
    Open,
    Stopping,
    ShuttingDown,
    Closed,
};

// 关闭不是一个布尔开关。准备任务、代理租约和内核资源各有完成条件；
// 暴露当前屏障，界面才能说明“正在等什么”以及是否需要显式恢复。
enum class StopBarrier : quint8 {
    None,
    Preparation,
    ProxyRestore,
    ProxyRecovery,
    KernelCleanup,
    KernelRecovery,
};

struct RuntimeCapabilities {
    bool activate = false;
    bool changeMode = false;
    bool selectProxy = false;
    bool setTun = false;
    bool request = false;
    bool enableSystemProxy = false;
    bool disableSystemProxy = false;
    bool recoverSystemProxy = false;
};

// 单次读取得到的会话视图。capabilities 由同一组状态推导，界面直接据此启用操作，
// 不应自行拼接 kernel、busy、TUN、租约等条件，否则容易与实际接纳规则不一致。
struct RuntimeStatus {
    KernelController::State kernel = KernelController::State::Stopped;
    KernelController::Resources resources = KernelController::Resources::Released;
    RuntimeOperation operation = RuntimeOperation::Idle;
    RuntimeLifecycle lifecycle = RuntimeLifecycle::Open;
    StopBarrier stopBarrier = StopBarrier::None;
    TransactionStage stage = TransactionStage::Idle;
    ConfigurationCertainty certainty = ConfigurationCertainty::NoConfiguration;
    RuntimeCapabilities capabilities;
    std::shared_ptr<const ActivationIdentity> active;
    std::shared_ptr<const ActivationIdentity> pending;
    std::optional<SystemProxyController::LeaseState> proxyLease;
    std::optional<SystemProxyController::State> proxyState;
    std::optional<SystemProxyController::Failure> proxyFailure;
    std::optional<runtime_config::routing_mode> routing;
    quint64 kernelEpoch = 0;
    qint64 runtimeRevision = 0;
    qint64 requestId = 0;
    quint32 processId = 0;
    quint16 controllerPort = 0;
    quint16 mixedPort = 0;
    bool rollingBack = false;
    bool maintenance = false;
    bool configLocked = false;
    bool tunActive = false;
    bool stopping = false;
    bool shuttingDown = false;
    bool proxyBusy = false;
    bool suspended = false;
    bool online = false;
    bool serviceManaged = false;
};

struct RuntimeChange {
    enum class Kind : quint8 {
        Mode,
        Selection,
    };
    Kind kind = Kind::Mode;
    QString value;
    QString group;
};

// 客户端跨模块协作的唯一入口。适配器报告事实，本类决定配置、系统代理、
// TUN 与环境恢复的顺序；界面提交意图或读取状态，不能绕过此处调度。
class RuntimeSession final : public QObject {
    Q_OBJECT
  public:
    explicit RuntimeSession(KernelOptions options,
                            SystemProxyOptions proxyOptions = SystemProxyOptions(),
                            EnvironmentOptions environment = EnvironmentOptions(),
                            QObject* parent = nullptr);
    ~RuntimeSession() override;
    RuntimeSession(const RuntimeSession&) = delete;
    RuntimeSession& operator=(const RuntimeSession&) = delete;
    RuntimeSession(RuntimeSession&&) = delete;
    RuntimeSession& operator=(RuntimeSession&&) = delete;
    [[nodiscard]] KernelController::State state() const noexcept;
    [[nodiscard]] quint32 processId() const noexcept;
    [[nodiscard]] quint16 controllerPort() const noexcept;
    [[nodiscard]] [[maybe_unused]] QString dataDirectory() const;
    [[nodiscard]] QString statusText() const;
    [[nodiscard]] const QString& version() const noexcept;
    [[nodiscard]] const TrafficController& traffic() const noexcept;
    [[nodiscard]] [[maybe_unused]] const SystemProxyController* systemProxy() const noexcept;
    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] [[maybe_unused]] bool uncertain() const noexcept;
    [[nodiscard]] [[maybe_unused]] quint16 mixedPort() const noexcept;
    [[nodiscard]] [[maybe_unused]] bool tunActive() const noexcept;
    [[nodiscard]] RuntimeStatus status() const noexcept;
    [[nodiscard]] [[maybe_unused]] auto lastTransaction() const noexcept
        -> std::shared_ptr<const TransactionResult>;
    // stop() 完成后可再次 start()；shutdown() 永久关闭本对象的接纳入口。
    // 拥有者退出前应等待 shutdownFinished，而不是用析构代替异步资源回收。
    void start();
    void stop();
    void shutdown();
    [[maybe_unused]] void setTrafficEnabled(bool enabled);
    [[nodiscard]] [[maybe_unused]] bool setSystemProxyEnabled(bool enabled);
    [[nodiscard]] [[maybe_unused]] bool recoverSystemProxy();
    [[nodiscard]] bool activate(ActivationInput input);
    [[nodiscard]] ActivationSubmission submitActivation(ActivationInput input);
    [[nodiscard]] [[maybe_unused]] bool setTunEnabled(bool enabled);
    [[nodiscard]] [[maybe_unused]] bool setMode(runtime_config::routing_mode mode);
    [[nodiscard]] [[maybe_unused]] bool selectProxy(const QString& group, const QString& name);
    [[nodiscard]] bool request(const QByteArray& path, const QByteArray& method = "GET",
                               const QByteArray& body = {});
    [[maybe_unused]] void setLogsEnabled(bool enabled);
  Q_SIGNALS:
    void operationChanged();
    void activationFinished(bool success, bool restored, const QString& message);
    void transactionFinished(const TransactionResult& result);
    void requestFinished(int status, const QByteArray& response);
    void logMessage(const QString& message);
    void environmentStatus(const QString& message);
    void shutdownFinished();

  private:
    enum class Execution : quint8 {
        Preparing,
        Switching,
        Changing,
        ActivationTun,
        Tun,
    };

    // 同一时刻只允许一份跨模块事务。execution 是内部执行步骤，result.stage
    // 是对外诊断阶段；二者分开，避免展示状态反过来驱动执行流程。
    struct PendingTransaction {
        TransactionResult result;
        Execution execution = Execution::Preparing;
        QString payload;
        bool enableTun = false;
    };

    struct TransactionToken {
        // 会话重建改变 epoch；同一会话内的新请求改变 requestId。
        // 两者都匹配时，异步回调才有权推进当前事务。
        quint64 epoch = 0;
        qint64 requestId = 0;
    };

    void checkFinished();
    void switched(const SwitchResult& result);
    void kernelChanged();
    void finishStop();
    void refreshStreams();
    void heal();
    void refreshProxy();
    [[nodiscard]] bool configLocked() const noexcept;
    [[nodiscard]] runtime_config::runtime_context context() const;
    [[nodiscard]] bool change(RuntimeChange requested);
    [[nodiscard]] bool canMutate() const noexcept;
    [[nodiscard]] bool canSetTun() const noexcept;
    [[nodiscard]] RuntimeOperation operation() const noexcept;
    [[nodiscard]] ActivationRejection activationRejection() const noexcept;
    [[nodiscard]] bool preparing() const noexcept;
    [[nodiscard]] bool stopping() const noexcept;
    [[nodiscard]] bool closing() const noexcept;
    [[nodiscard]] StopBarrier stopBarrier() const noexcept;
    [[nodiscard]] TransactionToken transactionToken() const noexcept;
    [[nodiscard]] bool currentTransaction(TransactionToken token) const noexcept;
    [[nodiscard]] TransactionResult& beginTransaction(TransactionKind kind);
    void issueTun(bool enabled);
    void finishTransaction(TransactionOutcome outcome, TransactionStage stage,
                           TransactionError error, const QString& message);
    void cancelTransaction();
    // 成员顺序也是依赖生命期顺序：执行步骤先于它观察的内核被销毁。
    KernelOptions options_;
    KernelController kernel_;
    ConfigSwitcher switcher_;
    TrafficController traffic_;
    EnvironmentMonitor environment_;
    CoreLogSocket logs_;
    std::unique_ptr<SystemProxyController> proxy_;
    QNetworkAccessManager network_;
    QPointer<QNetworkReply> reply_;
    QFutureWatcher<ActivationCheck> checker_;
    std::stop_source cancellation_;
    quint64 generation_ = 0;
    quint64 checkGeneration_ = 0;
    QString active_;
    QJsonObject selected_;
    std::shared_ptr<const ActivationIdentity> activeIdentity_;
    std::shared_ptr<const TransactionResult> lastTransaction_;
    std::optional<PendingTransaction> transaction_;
    std::optional<runtime_config::routing_mode> routing_;
    qint64 nextRequestId_ = 0;
    qint64 runtimeRevision_ = 0;
    quint16 mixedPort_ = 0;
    bool tunActive_ = false;
    bool uncertain_ = false;
    RuntimeLifecycle lifecycle_ = RuntimeLifecycle::Open;
    bool logsEnabled_ = false;
    bool suspended_ = false;
    bool healPending_ = false;
    bool healing_ = false;
    bool online_ = false;
    int healAttempts_ = 0;
    QTimer healRetry_;
};

#endif
