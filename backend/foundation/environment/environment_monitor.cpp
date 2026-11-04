#include <winsock2.h>
#include <ws2tcpip.h>

#include <windows.h>

#include <iphlpapi.h>

#include <Netioapi.h>
#include <PowrProf.h>
#include <QtConcurrentRun>
#include <blank/environment/environment_monitor.hpp>
#include <chrono>
#include <exception>
#include <stdexcept>
#include <system_error>
#include <tuple>
#include <utility>

namespace
{
void checkStatus(ULONG status)
{
    if (status != NO_ERROR)
    {
        throw std::system_error(static_cast<int>(status), std::system_category());
    }
}

void publishNetwork(void* context, MIB_NOTIFICATION_TYPE type) noexcept
{
    if (type != MibInitialNotification)
    {
        static_cast<EnvironmentBuffer*>(context)->publish(EnvironmentChange::Network);
    }
}

void CALLBACK interfaceChanged(void* context, MIB_IPINTERFACE_ROW* /*unused*/,
                               MIB_NOTIFICATION_TYPE type)
{
    publishNetwork(context, type);
}

void CALLBACK routeChanged(void* context, MIB_IPFORWARD_ROW2* /*unused*/,
                           MIB_NOTIFICATION_TYPE type)
{
    publishNetwork(context, type);
}

ULONG CALLBACK powerChanged(void* context, ULONG type, void* /*unused*/)
{
    auto* buffer = static_cast<EnvironmentBuffer*>(context);
    if (type == PBT_APMSUSPEND)
    {
        buffer->publish(EnvironmentChange::Suspending);
    }
    else if (type == PBT_APMRESUMESUSPEND || type == PBT_APMRESUMEAUTOMATIC)
    {
        buffer->publish(EnvironmentChange::Resumed);
    }
    return ERROR_SUCCESS;
}
} // namespace

class NativeEnvironmentNotifications final
{
  public:
    explicit NativeEnvironmentNotifications(std::shared_ptr<EnvironmentBuffer> buffer)
        : buffer_(std::move(buffer)),
          parameters_{.Callback = powerChanged, .Context = buffer_.get()}
    {
    }

    ~NativeEnvironmentNotifications()
    {
        const bool power =
            power_ == nullptr || UnregisterSuspendResumeNotification(power_) != FALSE;
        const bool routes = routes_ == nullptr || CancelMibChangeNotify2(routes_) == NO_ERROR;
        const bool interfaces =
            interfaces_ == nullptr || CancelMibChangeNotify2(interfaces_) == NO_ERROR;
        if (!power || !routes || !interfaces)
        {
            // Never free a context that Windows may still call. Owned-process guards recover on
            // exit.
            OutputDebugStringW(L"BlankQuick could not unregister environment callbacks.\n");
            TerminateProcess(GetCurrentProcess(), ERROR_INVALID_HANDLE);
            std::terminate();
        }
    }

    NativeEnvironmentNotifications(const NativeEnvironmentNotifications&) = delete;
    NativeEnvironmentNotifications& operator=(const NativeEnvironmentNotifications&) = delete;
    NativeEnvironmentNotifications(NativeEnvironmentNotifications&&) = delete;
    NativeEnvironmentNotifications& operator=(NativeEnvironmentNotifications&&) = delete;

    void start()
    {
        checkStatus(NotifyIpInterfaceChange(AF_UNSPEC, interfaceChanged, buffer_.get(), FALSE,
                                            &interfaces_));
        checkStatus(NotifyRouteChange2(AF_UNSPEC, routeChanged, buffer_.get(), FALSE, &routes_));
        power_ = RegisterSuspendResumeNotification(&parameters_, DEVICE_NOTIFY_CALLBACK);
        if (power_ == nullptr)
        {
            checkStatus(GetLastError());
            throw std::runtime_error("Power notification registration failed.");
        }
    }

  private:
    std::shared_ptr<EnvironmentBuffer> buffer_;
    DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS parameters_;
    HANDLE interfaces_ = nullptr;
    HANDLE routes_ = nullptr;
    HPOWERNOTIFY power_ = nullptr;
};

bool EnvironmentBatch::resumed() const noexcept
{
    return (kinds & static_cast<unsigned>(EnvironmentChange::Resumed)) != 0;
}

void EnvironmentBuffer::publish(EnvironmentChange change) noexcept
{
    if (change == EnvironmentChange::Suspending)
    {
        suspended_.store(true);
    }
    else if (change == EnvironmentChange::Resumed)
    {
        suspended_.store(false);
    }
    kinds_.fetch_or(static_cast<unsigned>(change));
}

void EnvironmentBuffer::retain(EnvironmentBatch batch) noexcept
{
    // A failed older observation must not overwrite a newer native suspend/resume state.
    kinds_.fetch_or(batch.kinds);
}

bool EnvironmentBuffer::pending() const noexcept
{
    return kinds_.load() != 0;
}

EnvironmentBatch EnvironmentBuffer::take() noexcept
{
    return {.kinds = kinds_.exchange(0), .suspended = suspended_.load()};
}

EnvironmentMonitor::EnvironmentMonitor(EnvironmentOptions options, QObject* parent)
    : QObject(parent), options_(std::move(options))
{
    if (options_.coalescingMs < 10 || options_.coalescingMs > 1000 || !options_.read)
    {
        throw std::invalid_argument("Invalid environment observation options.");
    }
    poll_.setInterval(std::chrono::milliseconds{100});
    coalescing_.setSingleShot(true);
    connect(&poll_, &QTimer::timeout, this,
            [this]
            {
                if (buffer_->pending() && !coalescing_.isActive() && !snapshot_.isRunning())
                {
                    coalescing_.start(options_.coalescingMs);
                }
            });
    connect(&coalescing_, &QTimer::timeout, this, &EnvironmentMonitor::sample);
    connect(&registration_, &QFutureWatcher<EnvironmentRegistration>::finished, this,
            [this]
            {
                auto result = registration_.future().takeResult();
                native_ = std::move(result.native);
                if (!result.error.isEmpty())
                {
                    Q_EMIT failed(result.error);
                }
                else
                {
                    // Close the observation/registration race with one post-registration snapshot.
                    buffer_->publish(EnvironmentChange::Network);
                    Q_EMIT registered();
                }
            });
    connect(&snapshot_, &QFutureWatcher<NetworkSnapshot>::finished, this,
            [this]
            {
                const auto result = snapshot_.future().takeResult();
                if (!result.error.isEmpty())
                {
                    Q_EMIT failed(result.error);
                    buffer_->retain(sampling_);
                    buffer_->publish(EnvironmentChange::Network);
                    coalescing_.start(std::chrono::milliseconds{1000});
                    return;
                }
                const bool changed = !fingerprint_.isEmpty() && fingerprint_ != result.fingerprint;
                fingerprint_ = result.fingerprint;
                Q_EMIT observed(result, sampling_, changed);
            });
}

EnvironmentMonitor::~EnvironmentMonitor()
{
    poll_.stop();
    coalescing_.stop();
    if (registration_.isFinished() && registration_.future().resultCount() > 0)
    {
        native_ = registration_.future().takeResult().native;
    }
    if (native_)
    {
        // Cancellation can wait for callbacks; callbacks never depend on the GUI or worker pool.
        std::ignore =
            QtConcurrent::run([native = std::move(native_)]() mutable { native.reset(); });
    }
}

void EnvironmentMonitor::start()
{
    if (started_)
    {
        return;
    }
    started_ = true;
    poll_.start();
    sample();
    if (options_.native)
    {
        const auto callbackBuffer = buffer_;
        registration_.setFuture(QtConcurrent::run(
            [callbackBuffer]
            {
                try
                {
                    auto native = std::make_shared<NativeEnvironmentNotifications>(callbackBuffer);
                    native->start();
                    return EnvironmentRegistration{.native = std::move(native), .error = {}};
                }
                catch (const std::exception& error)
                {
                    return EnvironmentRegistration{
                        .native = {},
                        .error = QString::fromUtf8(error.what()),
                    };
                }
            }));
    }
}

void EnvironmentMonitor::sample()
{
    if (!snapshot_.isRunning())
    {
        sampling_ = buffer_->take();
        const auto read = options_.read;
        snapshot_.setFuture(QtConcurrent::run(
            [read]
            {
                try
                {
                    return read();
                }
                catch (const std::exception& error)
                {
                    return NetworkSnapshot{
                        .fingerprint = {},
                        .online = false,
                        .error = QString::fromUtf8(error.what()),
                    };
                }
            }));
    }
}

std::shared_ptr<EnvironmentBuffer> EnvironmentMonitor::buffer() const
{
    return buffer_;
}
