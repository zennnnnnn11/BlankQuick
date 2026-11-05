#ifndef BLANK_QUICK_NATIVE_KERNEL_HPP
#define BLANK_QUICK_NATIVE_KERNEL_HPP

#include <QtTypes>

#include <QString>
#include <QStringList>
#include <memory>

struct KernelOptions {
    QString executable;
    QString signalHelper;
    QString workRoot{};
    quint16 port = 0;
    int startupTimeoutMs = 10000;
    bool serviceManaged = false;
};

// 独占一组原生资源：临时工作目录、配置、Job Object 和内核进程。
// processHandle() 只提供观察句柄，调用者不能关闭；实际释放由 Impl 的 RAII 负责。
// 该类型不决定用户配置和重试策略，只执行启动、归属核验与停止。
class NativeKernel final {
  public:
    explicit NativeKernel(const KernelOptions& options);
    ~NativeKernel();
    NativeKernel(const NativeKernel&) = delete;
    NativeKernel& operator=(const NativeKernel&) = delete;
    NativeKernel(NativeKernel&&) = delete;
    NativeKernel& operator=(NativeKernel&&) = delete;

    [[nodiscard]] void* processHandle() const noexcept;
    [[nodiscard]] quint32 processId() const noexcept;
    [[nodiscard]] quint16 port() const noexcept;
    [[nodiscard]] const QString& secret() const noexcept;
    [[nodiscard]] const QString& initialConfiguration() const noexcept;
    [[nodiscard]] QString workingDirectory() const;
    [[nodiscard]] QString providerCacheError() const;
    [[nodiscard]] QString stop(const QString& signalHelper);

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

struct KernelLaunch {
    std::shared_ptr<NativeKernel> kernel;
    QString error;
};

struct ListenerCheck {
    bool owned = false;
    QString error;
};

[[nodiscard]] KernelLaunch launchKernel(const KernelOptions& options);
[[nodiscard]] ListenerCheck checkKernelListener(quint16 port, quint32 processId);
[[nodiscard]] ListenerCheck checkProxyListener(quint16 port, quint32 processId);
[[nodiscard]] QString runOwnedTool(const QString& executable, const QStringList& arguments,
                                   const QString& directory, int timeoutMs);

#endif
