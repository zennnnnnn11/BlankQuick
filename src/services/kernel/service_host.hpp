#ifndef BLANK_QUICK_SERVICE_HOST_HPP
#define BLANK_QUICK_SERVICE_HOST_HPP

#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QScopeGuard>
#include <QWebSocket>
#include <blank/config/service_config.hpp>
#include <blank/environment/environment_monitor.hpp>
#include <blank/ipc/service_pipe.hpp>
#include <blank/process/native_kernel.hpp>
#include <blank/streams/core_log_socket.hpp>
#include <blank/tun/tun_resources.hpp>
#include <concepts>
#include <functional>
#include <optional>
#include <type_traits>
#include <utility>

// 提权执行边界，只管理本服务拥有的进程、TUN 与清理状态。
// 不读取客户端订阅或覆写存储，也不决定用户该启用哪份业务配置。
class ServiceHost final : public QObject {
    Q_OBJECT
  public:
    explicit ServiceHost(QObject* parent = nullptr);
    ~ServiceHost() override;
    ServiceHost(const ServiceHost&) = delete;
    ServiceHost& operator=(const ServiceHost&) = delete;
    ServiceHost(ServiceHost&&) = delete;
    ServiceHost& operator=(ServiceHost&&) = delete;
    void initialize();
    void shutdown();
  Q_SIGNALS:
    void ready();
    void drained();
    void fatal(const QString& message);

  private:
    enum class Operation : quint8 {
        Idle,
        Starting,
        VerifyVersion,
        InitialGeneral,
        InitialConfig,
        Api,
        Mode,
        EnvironmentDns,
        Compile,
        ConfigApply,
        ConfigGeneral,
        VerifyConfigTun,
        Prepare,
        Enable,
        VerifyTun,
        Disable,
        Cleanup,
        Stop,
    };
    enum class Cleanup : quint8 {
        Startup,
        TunOff,
        Stop,
    };
    enum class Repair : quint8 {
        Available,
        InProgress,
        Exhausted,
    };
    Q_ENUM(Operation)
    Q_ENUM(Cleanup)
    Q_ENUM(Repair)

    struct WorkToken {
        // 原生任务可能比取消请求更晚结束。只有会话、操作和资源身份
        // 全部一致，结果才能推进状态；过期资源仍须经过清理。
        quint64 kernelEpoch = 0;
        qint64 tunEpoch = 0;
        Operation operation = Operation::Idle;
        QString guid;
    };

    [[nodiscard]] WorkToken workToken() const noexcept;

    template <std::movable Result, std::invocable Start>
        requires std::same_as<std::invoke_result_t<Start>, QFuture<Result>>
    void startWork(QFutureWatcher<Result>& watcher, std::optional<WorkToken>& work, Start&& start) {
        // 先登记身份再启动任务。同步启动失败时作用域守卫撤销登记；成功后由
        // 完成回调取走 token。它既是结果准入凭据，也是停止流程等待任务的屏障。
        work = workToken();
        auto reset = qScopeGuard([&work]() noexcept { work.reset(); });
        watcher.setFuture(std::invoke(std::forward<Start>(start)));
        reset.dismiss();
    }

    void received(const QJsonObject& frame);
    void launchFinished();
    void readVersion();
    void sendHttp(const QByteArray& path, const QByteArray& method = "GET",
                  const QByteArray& body = {});
    void httpFinished(QNetworkReply* reply, quint64 generation);
    void forwardApi(const QJsonObject& arguments);
    void readConnections(const QString& id);
    void stop(const QString& id = {}, const QString& error = {});
    void stopProcess();
    void cleanup(Cleanup purpose);
    void cleanupFinished();
    void enableTun();
    void verifyTun();
    void finish(int status, const QByteArray& body = "{}", const QString& error = {});
    void respond(const QString& id, int status, const QByteArray& body, const QString& error = {});
    void openTraffic();
    void openLogs();
    void environmentObserved(const NetworkSnapshot& snapshot, EnvironmentBatch batch, bool changed);
    void checkEnvironment();
    void environmentHealthFinished();
    void reportEnvironment(const QString& message);
    [[nodiscard]] QByteArray information() const;
    SecurePipe pipe_;
    KernelOptions options_;
    std::shared_ptr<NativeKernel> core_;
    QFutureWatcher<KernelLaunch> launch_;
    QFutureWatcher<ListenerCheck> listener_;
    QFutureWatcher<QString> stopper_;
    QFutureWatcher<QString> cleaner_;
    QFutureWatcher<ServiceConfig> compiler_;
    QFutureWatcher<TunPlan> planner_;
    QFutureWatcher<TunPlan> observer_;
    QFutureWatcher<TunHealth> health_;
    EnvironmentMonitor environment_;
    QNetworkAccessManager network_;
    QPointer<QNetworkReply> reply_;
    QPointer<QNetworkReply> monitor_;
    std::unique_ptr<QWinEventNotifier> processEvent_;
    QWebSocket traffic_;
    CoreLogSocket logs_;
    QString logLevel_;
    QTimer trafficRetry_;
    QTimer trafficWatchdog_;
    QTimer environmentRetry_;
    QElapsedTimer healthFailure_;
    QTimer poll_;
    QTimer deadline_;
    QJsonObject runtimeConfig_;
    QJsonObject pendingConfig_;
    QString journal_;
    QString activeId_;
    QString version_;
    QString stopError_;
    QString environmentMessage_;
    quint16 mixedPort_ = 0;
    quint64 generation_ = 0;
    qint64 tunGeneration_ = 0;
    std::optional<WorkToken> launchWork_;
    std::optional<WorkToken> listenerWork_;
    std::optional<WorkToken> compilerWork_;
    std::optional<WorkToken> plannerWork_;
    std::optional<WorkToken> observerWork_;
    std::optional<WorkToken> stopWork_;
    std::optional<WorkToken> cleanupWork_;
    std::optional<WorkToken> healthWork_;
    quint32 environmentChecks_ = 0;
    quint32 dnsFlushes_ = 0;
    quint32 tunRepairs_ = 0;
    int dnsAttempts_ = 0;
    int healthAttempts_ = 0;
    TunPlan plan_;
    Operation operation_ = Operation::Idle;
    Cleanup cleanup_ = Cleanup::Startup;
    bool trafficRequested_ = false;
    bool tunActive_ = false;
    bool stopping_ = false;
    bool serviceStopping_ = false;
    bool blocked_ = false;
    bool suspended_ = false;
    bool online_ = false;
    bool refreshPending_ = false;
    bool dnsPending_ = false;
    bool healthPending_ = false;
    Repair repair_ = Repair::Available;
};

#endif
