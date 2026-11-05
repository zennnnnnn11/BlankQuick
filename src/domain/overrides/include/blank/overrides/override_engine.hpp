#ifndef BLANK_QUICK_OVERRIDE_ENGINE_HPP // NOLINT(llvm-header-guard)
#define BLANK_QUICK_OVERRIDE_ENGINE_HPP

#include <blank/overrides/override_types.hpp>
#include <chrono>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace overrides {

enum class configuration_format : std::int8_t { json, yaml };

inline constexpr std::int64_t default_document_bytes{33554432};
inline constexpr std::int64_t default_patch_bytes{2097152};
inline constexpr std::int64_t default_value_limit{1000000};
inline constexpr std::int64_t default_patch_limit{128};
inline constexpr std::int64_t default_log_lines{128};
inline constexpr std::int64_t default_log_bytes{16384};
inline constexpr std::int64_t default_document_depth{64};
inline constexpr std::chrono::milliseconds default_script_timeout{2000};

// NOLINTBEGIN(altera-struct-pack-align,readability-redundant-member-init)
struct engine_options {
    configuration_format input_format{configuration_format::json};
    std::int64_t max_document_bytes{default_document_bytes};
    std::int64_t max_patch_bytes{default_patch_bytes};
    std::int64_t max_values{default_value_limit};
    std::int64_t max_depth{default_document_depth};
    std::int64_t max_patches{default_patch_limit};
    std::int64_t max_log_lines{default_log_lines};
    std::int64_t max_log_bytes{default_log_bytes};
    std::chrono::milliseconds script_timeout{default_script_timeout};
};

struct patch_report {
    std::string id{};
    override_error error{override_error::none};
    std::string message{};
    std::vector<std::string> logs{};
    bool logs_truncated{};
};

struct apply_result {

    std::string config_json{};
    std::vector<patch_report> reports{};
    override_error error{override_error::none};
};
// NOLINTEND(altera-struct-pack-align,readability-redundant-member-init)

// NOLINTBEGIN(fuchsia-default-arguments-declarations)
[[nodiscard]] [[maybe_unused]] auto apply_overrides(std::string_view configuration,
                                                    std::span<const override_item> patches,
                                                    const engine_options& options = {}) noexcept
    -> apply_result;
// NOLINTEND(fuchsia-default-arguments-declarations)

} // namespace overrides

#endif
