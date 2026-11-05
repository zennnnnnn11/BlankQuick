#ifndef BLANK_QUICK_SETTINGS_CODEC_HPP // NOLINT(llvm-header-guard)
#define BLANK_QUICK_SETTINGS_CODEC_HPP

#include <blank/runtime_config/runtime_config.hpp>
#include <blank/shared/json_document.hpp>
#include <concepts>
#include <optional>

namespace runtime_config {
// NOLINTBEGIN(altera-struct-pack-align,readability-redundant-member-init,fuchsia-default-arguments-calls)
template <std::default_initializable Value> struct settings_codec_result {
    // 错误结果不构造 Value，避免在内存不足的处理路径中再次分配
    // 默认设置所需的字符串和 DNS 数组。
    std::optional<Value> value{};
    configuration_error error{configuration_error::none};
};
// NOLINTEND(altera-struct-pack-align,readability-redundant-member-init,fuchsia-default-arguments-calls)

// The storage adapter owns the envelope/revision/atomic write. This codec owns
// only the settings payload, using exactly the generation rules without I/O.
[[nodiscard]] auto encode_settings(const client_settings& settings) noexcept
    -> settings_codec_result<json_detail::document>;
[[nodiscard]] auto decode_settings(const json_detail::document& document) noexcept
    -> settings_codec_result<client_settings>;
} // namespace runtime_config

#endif
