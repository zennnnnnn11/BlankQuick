#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QProcess>
#include <QtCore/QProcessEnvironment>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QTemporaryDir>

#include <algorithm>
#include <blank/runtime_config/kernel_check.hpp>
#include <blank/shared/timeouts.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <new>
#include <qfiledevice.h>
#include <qnamespace.h>
#include <stop_token>
#include <string>
#include <string_view>

namespace runtime_config {
namespace {
// NOLINTBEGIN(llvm-prefer-static-over-anonymous-namespace,fuchsia-default-arguments-calls,altera-unroll-loops)
constexpr std::size_t maximum_candidate_bytes{33554432};
constexpr auto maximum_timeout_ms = timeout_budget::native_check.count();
constexpr std::int64_t maximum_output_bytes{1048576};
constexpr int poll_interval_ms{25};
constexpr int shutdown_timeout_ms{1000};

[[nodiscard]] auto qt_path(const std::filesystem::path& path) -> QString {
    return QString::fromStdWString(path.native());
}

auto collect_output(QProcess& process, const kernel_check_options& options,
                    kernel_check_result& result) -> void {
    const auto bytes = process.readAllStandardOutput();
    const auto remaining = options.max_output_bytes - std::ssize(result.output);
    const auto count = std::min<std::int64_t>(bytes.size(), remaining);
    result.output.append(bytes.constData(), static_cast<std::size_t>(count));
    result.output_truncated = result.output_truncated || bytes.size() > count;
}

[[nodiscard]] auto run_check(std::string_view candidate, const kernel_check_options& options,
                             const std::stop_token& stop) -> kernel_check_result {
    // 真正启动指定内核的 -t 子进程，属于副作用边界。候选写入独立临时目录，
    // 不覆盖运行配置；输出、总时间和取消响应均有上限，退出前回收本次子进程。
    if (candidate.empty() || candidate.size() > maximum_candidate_bytes ||
        !options.executable.is_absolute() || !options.data_directory.is_absolute() ||
        !std::filesystem::is_regular_file(options.executable) ||
        !std::filesystem::is_directory(options.data_directory) || options.timeout.count() < 1 ||
        options.timeout.count() > maximum_timeout_ms || options.max_output_bytes < 1 ||
        options.max_output_bytes > maximum_output_bytes) {
        return {.error = kernel_check_error::invalid_input};
    }
    if (stop.stop_requested()) {
        return {.error = kernel_check_error::cancelled};
    }
    auto const directory = QTemporaryDir{};
    if (!directory.isValid()) {
        return {.error = kernel_check_error::io_failure};
    }
    const auto path = directory.filePath(QStringLiteral("candidate.json"));
    auto file = QFile{path};
    if (!file.open(QIODevice::WriteOnly) ||

        // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage)
        file.write(candidate.data(), std::ssize(candidate)) != std::ssize(candidate)) {
        return {.error = kernel_check_error::io_failure};
    }
    file.close();
    auto process = QProcess{};
    process.setProcessChannelMode(QProcess::MergedChannels);
    auto environment = QProcessEnvironment::systemEnvironment();
    // 清理可能改变 Mihomo 配置语义或路径准入的继承变量，确保预检的是参数中
    // 指定的候选和目录，而不是宿主环境悄悄替换的配置。
    for (const auto& key : environment.keys()) {
        if (key.startsWith(QStringLiteral("CLASH_"), Qt::CaseInsensitive) ||
            key.compare(QStringLiteral("SKIP_SAFE_PATH_CHECK"), Qt::CaseInsensitive) == 0 ||
            key.compare(QStringLiteral("SAFE_PATHS"), Qt::CaseInsensitive) == 0) {
            environment.remove(key);
        }
    }
    process.setProcessEnvironment(environment);
    process.setCreateProcessArgumentsModifier(
        [](QProcess::CreateProcessArguments* arguments) -> void {
            constexpr std::uint32_t no_window{0x08000000UL};
            arguments->flags |= no_window;
        });
    const auto deadline = std::chrono::steady_clock::now() + options.timeout;
    process.start(qt_path(options.executable), QStringList{
                                                   QStringLiteral("-t"),
                                                   QStringLiteral("-d"),
                                                   qt_path(options.data_directory),
                                                   QStringLiteral("-f"),
                                                   path,
                                               });
    auto result = kernel_check_result{};
    while (process.state() != QProcess::NotRunning) {
        static_cast<void>(process.waitForFinished(poll_interval_ms));
        collect_output(process, options, result);
        if (stop.stop_requested() || std::chrono::steady_clock::now() >= deadline) {
            result.error =
                stop.stop_requested() ? kernel_check_error::cancelled : kernel_check_error::timeout;
            process.kill();
            static_cast<void>(process.waitForFinished(shutdown_timeout_ms));
            collect_output(process, options, result);
            return result;
        }
    }
    collect_output(process, options, result);
    if (process.error() == QProcess::FailedToStart) {
        result.error = kernel_check_error::start_failed;
    } else {
        result.exit_code = process.exitCode();
        if (process.exitStatus() != QProcess::NormalExit || result.exit_code != 0) {
            result.error = kernel_check_error::rejected;
        }
    }
    return result;
}
// NOLINTEND(llvm-prefer-static-over-anonymous-namespace,fuchsia-default-arguments-calls,altera-unroll-loops)
} // namespace

auto check_with_kernel(std::string_view candidate, const kernel_check_options& options,
                       const std::stop_token& stop) noexcept -> kernel_check_result {
    try {
        return run_check(candidate, options, stop);
    } catch (const std::filesystem::filesystem_error&) {
        return {.error = kernel_check_error::io_failure};
    } catch (const std::bad_alloc&) {
        return {.error = kernel_check_error::allocation_failure};
    } catch (const std::exception&) {
        return {.error = kernel_check_error::unexpected_failure};
    } catch (...) {
        return {.error = kernel_check_error::unexpected_failure};
    }
}
} // namespace runtime_config
