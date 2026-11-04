#ifndef BLANK_RUNTIME_SESSION_HPP
#define BLANK_RUNTIME_SESSION_HPP

#include "config_switcher.hpp"
#include "runtime_config.hpp"
#include "runtime_contract.hpp"
#include "system_proxy_controller.hpp"
#include "traffic_controller.hpp"
#include <QFutureWatcher>
#include <QNetworkAccessManager>
#include <QPointer>
#include <blank/environment/environment_monitor.hpp>
#include <blank/streams/core_log_socket.hpp>
#include <memory>
#include <stop_token>

enum class RuntimeOperation : quint8
{
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

enum class ConfigurationCertainty : quint8
{
    NoConfiguration,
    Confirmed,
    Uncertain,
};

enum class RuntimeLifecycle : quint8
{
    Open,
    Stopping,
    ShuttingDown,
    Closed,
};

enum class StopBarrier : quint8
{
    None,
    Preparation,
    ProxyRestore,
    ProxyRecovery,
    KernelCleanup,
    KernelRecovery,
};

struct RuntimeCapabilities
{
    bool activate = false;
    bool changeMode = false;
    bool selectProxy = false;
    bool setTun = false;
    bool request = false;
    bool enableSystemProxy = false;
    bool disableSystemProxy = false;
    bool recoverSystemProxy = false;
};

struct RuntimeStatus
{
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

struct ActivationCheck
{
    QString payload;
    QString error;
    bool enableTun = false;
    TransactionStage stage = TransactionStage::Generation;
    TransactionError code = TransactionError::None;
    ActivationDiagnostics diagnostics;
};

struct RuntimeChange
{
    enum class Kind : quint8
    {
        Mode,
        Selection,
    };
    Kind kind = Kind::Mode;
    QString value;
    QString group;
};

// QObject event-loop adapter. No window, QML registration or presentation model.
// Call methods on its owning thread. Config generation and native -t run on workers.
class RuntimeSession final : public QObject
{
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
    [[nodiscard]] QString dataDirectory() const;
    [[nodiscard]] QString statusText() const;
    [[nodiscard]] const QString& version() const noexcept;
    [[nodiscard]] const TrafficController& traffic() const noexcept;
    [[nodiscard]] const SystemProxyController* systemProxy() const noexcept;
    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] bool uncertain() const noexcept;
    [[nodiscard]] quint16 mixedPort() const noexcept;
    [[nodiscard]] bool tunActive() const noexcept;
    [[nodiscard]] RuntimeStatus status() const noexcept;
    [[nodiscard]] std::shared_ptr<const TransactionResult> lastTransaction() const noexcept;
    void start();
    void stop();
    void shutdown();
    void setTrafficEnabled(bool enabled);
    [[nodiscard]] bool setSystemProxyEnabled(bool enabled);
    [[nodiscard]] bool recoverSystemProxy();
    [[nodiscard]] bool activate(ActivationInput input);
    [[nodiscard]] ActivationSubmission submitActivation(ActivationInput input);
    [[nodiscard]] bool setTunEnabled(bool enabled);
    [[nodiscard]] bool setMode(runtime_config::routing_mode mode);
    [[nodiscard]] bool selectProxy(const QString& group, const QString& name);
    [[nodiscard]] bool request(const QByteArray& path, const QByteArray& method = "GET",
                               const QByteArray& body = {});
    void setLogsEnabled(bool enabled);
  Q_SIGNALS:
    void operationChanged();
    void activationFinished(bool success, bool restored, const QString& message);
    void transactionFinished(const TransactionResult& result);
    void requestFinished(int status, const QByteArray& response);
    void logMessage(const QString& message);
    void environmentStatus(const QString& message);
    void shutdownFinished();

  private:
    enum class Execution : quint8
    {
        Preparing,
        Switching,
        Changing,
        ActivationTun,
        Tun,
    };

    struct PendingTransaction
    {
        TransactionResult result;
        Execution execution = Execution::Preparing;
        QString payload;
        bool enableTun = false;
    };

    struct TransactionToken
    {
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
