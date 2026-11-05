#include "kernel_workspace.hpp"

#include <QDeadlineTimer>
#include <QDir>
#include <exception>
#include <stdexcept>
#include <utility>

namespace {
bool jobEmpty(HANDLE job) {
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
    blank::windows::check(QueryInformationJobObject(job, JobObjectBasicAccountingInformation,
                                                    &accounting, sizeof(accounting),
                                                    nullptr) != FALSE);
    return accounting.ActiveProcesses == 0;
}
} // namespace

namespace blank::process_detail {

KernelWorkspace::KernelWorkspace(const QString& root, windows::Handle&& job)
    : directory_((root.isEmpty() ? QDir::tempPath() : root) +
                 QStringLiteral("/blank-quick-XXXXXX")),
      job_(std::move(job)) {
    if (!directory_.isValid() || !job_) {
        throw std::runtime_error("Cannot create the kernel runtime workspace.");
    }
    // 必须在任何进程启动前关闭 Qt 的无条件析构清理。正常停止、构造回滚和
    // 最终析构统一走下面的 Job 确认；错误/超时路径不会再被 Qt 偷偷删目录。
    directory_.setAutoRemove(false);
}

KernelWorkspace::~KernelWorkspace() noexcept {
    try {
        // 不能仅检查主进程：它可能已经退出，而后代仍在使用目录。
        // 此析构也覆盖 NativeKernel 构造中“进程已启动但后续核验失败”的回滚。
        if (!jobEmpty(job_.get())) {
            windows::check(TerminateJobObject(job_.get(), 1) != FALSE);
        }
        removeAfterJobExit();
    } catch (const std::exception&) {
        // 无法确认 Job 为空时保留目录。立即关闭 Job，保留原来的
        // kill-on-close 兜底，但关闭句柄本身绝不能成为删除目录的依据。
        job_.reset();
    }
}

const QTemporaryDir& KernelWorkspace::directory() const noexcept {
    return directory_;
}

HANDLE KernelWorkspace::job() const noexcept {
    return job_.get();
}

void KernelWorkspace::removeAfterJobExit() {
    bool confirmedEmpty = false;
    const QDeadlineTimer deadline(2000);
    while (!deadline.hasExpired()) {
        confirmedEmpty = jobEmpty(job_.get());
        if (confirmedEmpty) {
            // 删除失败仍允许析构时重试；成功后不再碰这个路径，避免重复停止
            // 或延后的析构误删后来在同一路径新建的目录。
            if (removed_) {
                return;
            }
            // Job 记账归零与 Windows 完成文件/目录句柄回收之间可能有短暂间隔。
            // 直接使用可返回失败的目录 API 做有界重试，避免 QTemporaryDir::remove
            // 对可恢复的占用立即发出警告（QT_FATAL_WARNINGS 下会中断进程）。
            removed_ = QDir(directory_.path()).removeRecursively();
            if (removed_) {
                return;
            }
        }
        Sleep(10);
    }
    // 目录仍被外部占用不等于内核停止失败；保留原来的停止语义，析构可再试。
    if (confirmedEmpty) {
        return;
    }
    throw std::runtime_error("Timed out waiting for the owned kernel job to exit.");
}

} // namespace blank::process_detail
