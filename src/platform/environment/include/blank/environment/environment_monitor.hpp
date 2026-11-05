#ifndef BLANK_QUICK_ENVIRONMENT_MONITOR_HPP
#define BLANK_QUICK_ENVIRONMENT_MONITOR_HPP

#include <QTimer>

#include <QFutureWatcher>
#include <QObject>
#include <atomic>
#include <blank/environment/network_inventory.hpp>
#include <functional>
#include <memory>

enum class EnvironmentChange : quint8 {
    Network = 1,
    Suspending = 2,
    Resumed = 4,
};

struct EnvironmentBatch {
    unsigned kinds = 0;
    bool suspended = false;
    [[nodiscard]] bool resumed() const noexcept;
};

class EnvironmentBuffer final {
  public:
    void publish(EnvironmentChange change) noexcept;
    void retain(EnvironmentBatch batch) noexcept;
    [[nodiscard]] bool pending() const noexcept;
    [[nodiscard]] EnvironmentBatch take() noexcept;

  private:
    std::atomic<unsigned> kinds_{0};
    std::atomic<bool> suspended_{false};
};

class NativeEnvironmentNotifications;

struct EnvironmentRegistration {
    std::shared_ptr<NativeEnvironmentNotifications> native;
    QString error;
};

struct EnvironmentOptions {
    bool native = true;
    int coalescingMs = 750;
    std::function<NetworkSnapshot()> read = readNetworkSnapshot;
};

class EnvironmentMonitor final : public QObject {
    Q_OBJECT
  public:
    explicit EnvironmentMonitor(EnvironmentOptions options = EnvironmentOptions(),
                                QObject* parent = nullptr);
    ~EnvironmentMonitor() override;
    EnvironmentMonitor(const EnvironmentMonitor&) = delete;
    EnvironmentMonitor& operator=(const EnvironmentMonitor&) = delete;
    EnvironmentMonitor(EnvironmentMonitor&&) = delete;
    EnvironmentMonitor& operator=(EnvironmentMonitor&&) = delete;
    void start();
    [[nodiscard]] [[maybe_unused]] auto buffer() const noexcept
        -> std::shared_ptr<EnvironmentBuffer>;
  Q_SIGNALS:
    void observed(const NetworkSnapshot& snapshot, EnvironmentBatch batch, bool changed);
    void failed(const QString& error);
    void registered();

  private:
    void sample();
    EnvironmentOptions options_;
    std::shared_ptr<EnvironmentBuffer> buffer_ = std::make_shared<EnvironmentBuffer>();
    std::shared_ptr<NativeEnvironmentNotifications> native_;
    QFutureWatcher<EnvironmentRegistration> registration_;
    QFutureWatcher<NetworkSnapshot> snapshot_;
    QTimer poll_;
    QTimer coalescing_;
    EnvironmentBatch sampling_;
    QByteArray fingerprint_;
    bool started_ = false;
};

#endif
