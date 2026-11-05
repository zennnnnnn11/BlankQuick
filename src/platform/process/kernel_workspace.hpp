#ifndef BLANK_QUICK_KERNEL_WORKSPACE_HPP
#define BLANK_QUICK_KERNEL_WORKSPACE_HPP

#include <windows.h>

#include <QTemporaryDir>

#include <QString>
#include <blank/windows/handle.hpp>

namespace blank::process_detail {

// Job 与工作目录共用一个生命周期：句柄关闭会请求终止进程，但不能证明
// 进程已释放文件。目录删除始终需要另一次“整个 Job 已空”的确认。
// 仅供进程适配器内部使用；传入的 Job 应尚未启动进程并已设置 kill-on-close。
class KernelWorkspace final {
  public:
    KernelWorkspace(const QString& root, windows::Handle&& job);
    ~KernelWorkspace() noexcept;
    KernelWorkspace(const KernelWorkspace&) = delete;
    KernelWorkspace& operator=(const KernelWorkspace&) = delete;
    KernelWorkspace(KernelWorkspace&&) = delete;
    KernelWorkspace& operator=(KernelWorkspace&&) = delete;

    [[nodiscard]] const QTemporaryDir& directory() const noexcept;
    [[nodiscard]] HANDLE job() const noexcept;
    void removeAfterJobExit();

  private:
    QTemporaryDir directory_;
    windows::Handle job_;
    bool removed_ = false;
};

} // namespace blank::process_detail

#endif
