#ifndef BLANK_QUICK_RUNTIME_CONFIG_DETAIL_HPP // NOLINT(llvm-header-guard)
#define BLANK_QUICK_RUNTIME_CONFIG_DETAIL_HPP

#include <blank/runtime_config/runtime_config.hpp>
#include <blank/shared/json_document.hpp>
#include <exception>
#include <span>

namespace runtime_config::detail {
using json = json_detail::document;

// NOLINTNEXTLINE(altera-struct-pack-align)
class configuration_failure final : public std::exception {
  public:
    explicit configuration_failure(configuration_error selected) noexcept : error{selected} {}
    [[nodiscard]] auto code() const noexcept -> configuration_error {
        return error;
    }

  private:
    configuration_error error{};
};

[[noreturn]] inline auto fail(configuration_error error) -> void {
    throw configuration_failure{error};
}

[[nodiscard]] auto export_node(const node_model::unified_node& node) -> json;
[[nodiscard]] auto export_material(const profile_payload::minimal_profile& material,
                                   const generation_limits& limits) -> json;
[[nodiscard]] auto export_material(const profile_payload::minimal_profile_snapshot& material,
                                   const generation_limits& limits) -> json;
[[nodiscard]] auto settings_document(const client_settings& settings) -> json;
[[nodiscard]] auto settings_from_document(const json& document) -> client_settings;
auto validate_settings(const client_settings& settings) -> void;
auto validate_final(const json& document, const runtime_context& context,
                    const generation_limits& limits, effective_configuration& effective) -> void;
[[nodiscard]] auto base_document(const client_settings& settings, const runtime_context& context)
    -> json;
} // namespace runtime_config::detail

#endif
