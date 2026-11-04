#ifndef BLANK_QUICK_KERNEL_CONTROLLER_HPP
#define BLANK_QUICK_KERNEL_CONTROLLER_HPP

#include <QFutureWatcher>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <blank/process/native_kernel.hpp>
#include <memory>

class QNetworkReply;
class QWinEventNotifier;
class ServiceClient;

class KernelController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(State state READ state NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusChanged FINAL)
    Q_PROPERTY(QString version READ version NOTIFY statusChanged FINAL)
    Q_PROPERTY(Resources resources READ resources NOTIFY statusChanged FINAL)

  public:
    enum class State : quint8 {
        Stopped,
        Starting,
        WaitingForApi,
        Ready,
        Stopping,
        Failed,
    };
    Q_ENUM(State)
    enum class Resources : quint8 {
        Released,
        Pending,
        Owned,
        Uncertain,
    };
    Q_ENUM(Resources)
    explicit KernelController(KernelOptions options, QObject* parent = nullptr);
    ~KernelController() override;
    KernelController(const KernelController&) = delete;
    KernelController& operator=(const KernelController&) = delete;
    KernelController(KernelController&&) = delete;
    KernelController& operator=(KernelController&&) = delete;
    [[nodiscard]] State state() const noexcept;
    [[nodiscard]] QString statusText() const;
    [[nodiscard]] const QString& version() const noexcept;
    [[nodiscard]] quint32 processId() const noexcept;
    [[nodiscard]] quint16 port() const noexcept;
    [[nodiscard]] QString initialConfiguration() const;
    [[nodiscard]] QString homeDirectory() const;
    [[nodiscard]] QString sessionSecret() const;
    [[nodiscard]] QString controllerAddress() const;
    [[nodiscard]] QNetworkRequest apiRequest(const QByteArray& target) const;
    [[nodiscard]] QNetworkReply* sendRequest(QNetworkAccessManager& network,
                                             const QByteArray& target, const QByteArray& body = {},
                                             const QByteArray& method = "GET");
    [[nodiscard]] QNetworkReply* serviceRequest(const QString& operation,
                                                const QJsonObject& arguments = {});
    [[nodiscard]] bool serviceManaged() const noexcept;
    [[nodiscard]] bool maintenance() const noexcept;
    [[nodiscard]] Resources resources() const noexcept;
    void useService(bool enabled);
    void start();
    void stop();
    void shutdown();

  Q_SIGNALS:
    void stateChanged();
    void statusChanged();
    void shutdownFinished();
    void serviceTraffic(const QString& message);
    void serviceLogs(const QString& message);
    void serviceLogState(bool connected, const QString& message);
    void serviceEnvironment(const QString& message);
    void maintenanceChanged();

  private:
    struct PublishedStatus {
        State state = State::Stopped;
        Resources resources = Resources::Released;
        quint32 pid = 0;
        QString version;
        QString error;
        bool maintenance = false;
        bool serviceManaged = false;
        bool operator==(const PublishedStatus&) const = default;
    };
    [[nodiscard]] PublishedStatus currentStatus() const;
    void publishStatus();
    void setState(State nextState);
    void fail(const QString& error);
    void launchFinished();
    void probe();
    void listenerChecked();
    void requestVersion();
    void versionFinished(QNetworkReply* reply, quint64 generation);
    void beginCleanup();
    void cleanupFinished();
    void finishStopped();
    void connectService();
    void setMaintenance(bool enabled);

    KernelOptions options_;
    State state_ = State::Stopped;
    QString error_;
    QString version_;
    QString pendingVersion_;
    quint64 generation_ = 0;
    quint64 listenerGeneration_ = 0;
    bool launchPending_ = false;
    bool quitting_ = false;
    std::shared_ptr<NativeKernel> kernel_;
    std::unique_ptr<QWinEventNotifier> processNotifier_;
    QNetworkAccessManager network_;
    QPointer<QNetworkReply> reply_;
    QTimer poll_;
    QTimer deadline_;
    QFutureWatcher<KernelLaunch> launch_;
    QFutureWatcher<ListenerCheck> listener_;
    QFutureWatcher<QString> cleanup_;
    std::unique_ptr<ServiceClient> service_;
    quint32 servicePid_ = 0;
    Resources serviceResources_ = Resources::Released;
    bool maintenance_ = false;
    PublishedStatus published_;
    State notifiedState_ = State::Stopped;
    bool notifiedMaintenance_ = false;
};

#endif
