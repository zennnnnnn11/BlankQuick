#ifndef BLANK_QUICK_KERNEL_CHECK_HPP // NOLINT(llvm-header-guard)
#define BLANK_QUICK_KERNEL_CHECK_HPP

#include <blank/shared/timeouts.hpp>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <stop_token>
#include <string>
#include <string_view>

namespace runtime_config {

// NOLINTBEGIN(altera-struct-pack-align,readability-redundant-member-init,fuchsia-default-arguments-declarations)
inline constexpr auto default_kernel_timeout = timeout_budget::native_check;
inline constexpr std::int64_t default_kernel_output_bytes{65536};

enum class kernel_check_error : std::int8_t {
    none,
    invalid_input,
    start_failed,
    rejected,
    timeout,
    cancelled,
    io_failure,
    allocation_failure,
    unexpected_failure,
};

struct kernel_check_options {
    std::filesystem::path executable{};
    std::filesystem::path data_directory{};
    std::chrono::milliseconds timeout{default_kernel_timeout};
    std::int64_t max_output_bytes{default_kernel_output_bytes};
};

struct kernel_check_result {
    kernel_check_error error{kernel_check_error::none};
    std::int64_t exit_code{};
    std::string output{};
    bool output_truncated{};
};
[[nodiscard]] auto check_with_kernel(std::string_view candidate,
                                     const kernel_check_options& options,
                                     const std::stop_token& stop = {}) noexcept
    -> kernel_check_result;
// NOLINTEND(altera-struct-pack-align,readability-redundant-member-init,fuchsia-default-arguments-declarations)
} // namespace runtime_config

#endif
