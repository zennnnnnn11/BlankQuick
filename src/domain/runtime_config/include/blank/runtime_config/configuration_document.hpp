#ifndef BLANK_QUICK_CONFIGURATION_DOCUMENT_HPP // NOLINT(llvm-header-guard)
#define BLANK_QUICK_CONFIGURATION_DOCUMENT_HPP

#include <blank/overrides/override_dom.hpp>
#include <blank/runtime_config/runtime_config.hpp>

namespace runtime_config {

// A draft is deliberately not an applicable configuration. The application may
// run scripts between patches; only finalize_configuration validates the complete
// chain, restores client-owned invariants and publishes effective settings.
// NOLINTBEGIN(altera-struct-pack-align,readability-redundant-member-init,fuchsia-default-arguments-calls)
struct configuration_draft {
    json_detail::document value{};
    configuration_result status{};
};
// NOLINTEND(altera-struct-pack-align,readability-redundant-member-init,fuchsia-default-arguments-calls)

[[nodiscard]] auto make_draft(const profile_payload::minimal_profile& material,
                              const client_settings& settings, const runtime_context& context,
                              const generation_limits& limits) noexcept -> configuration_draft;
[[nodiscard]] auto make_draft(const profile_payload::minimal_profile_snapshot& material,
                              const client_settings& settings, const runtime_context& context,
                              const generation_limits& limits) noexcept -> configuration_draft;
[[nodiscard]] auto
finalize_configuration(overrides::dom::document_result&& applied, const client_settings& settings,
                       const runtime_context& context, const generation_limits& limits) noexcept
    -> configuration_result;
} // namespace runtime_config

#endif
