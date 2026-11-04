#include "runtime_config.hpp"

#include <concepts>
#include <exception>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "minimal_profile.hpp"
#include "override_dom.hpp"
#include "override_engine.hpp"
#include "override_types.hpp"
#include "runtime_config_detail.hpp"

namespace runtime_config {
namespace {
// CPU code has no FPGA unroll requirement.
// NOLINTBEGIN(llvm-prefer-static-over-anonymous-namespace,fuchsia-default-arguments-calls,altera-unroll-loops)
template <typename Material>
    requires std::same_as<Material, profile_payload::minimal_profile> ||
             std::same_as<Material, profile_payload::minimal_profile_snapshot>
auto compose(const Material& material, const client_settings& settings,
             const runtime_context& context, std::span<const overrides::override_item> patches,
             const generation_limits& limits, configuration_result& result, std::string_view& stage)
    -> void {
    if (limits.max_nodes < 1 || limits.max_groups < 1 || limits.max_rules < 1 ||
        limits.max_references < 1 ||
        limits.patches.input_format != overrides::configuration_format::json) {
        detail::fail(configuration_error::invalid_settings);
    }
    stage = "settings";
    auto candidate = detail::base_document(settings, context);
    stage = "material";
    auto business = detail::export_material(material, limits);
    for (auto& [key, value] : business.items()) {
        const auto field = std::string{key};
        candidate.emplace(field, std::move(value));
    }
    stage = "patches";
    auto applied = overrides::dom::apply_document(std::move(candidate), patches, limits.patches);
    result.patch_reports = std::move(applied.reports);
    if (applied.error == overrides::override_error::invalid_document &&
        applied.stage == overrides::dom::failure_stage::input) {
        detail::fail(configuration_error::invalid_document);
    }
    if (applied.error != overrides::override_error::none) {
        detail::fail(configuration_error::patch_failed);
    }
    stage = "configuration";
    detail::validate_final(applied.value, context, limits, result.effective);
    result.config_json = applied.value.dump();
}

template <typename Material>
    requires std::same_as<Material, profile_payload::minimal_profile> ||
             std::same_as<Material, profile_payload::minimal_profile_snapshot>
[[nodiscard]] auto guarded(const Material& material, const client_settings& settings,
                           const runtime_context& context,
                           std::span<const overrides::override_item> patches,
                           const generation_limits& limits) noexcept -> configuration_result {
    auto result = configuration_result{};
    auto stage = std::string_view{"limits"};
    try {
        compose(material, settings, context, patches, limits, result, stage);
    } catch (const detail::configuration_failure& failure) {
        result.error = failure.code();
    } catch (const detail::json::exception&) {
        result.error = configuration_error::invalid_document;
    } catch (const std::bad_alloc&) {
        result.error = configuration_error::allocation_failure;
    } catch (const std::exception&) {
        result.error = configuration_error::unexpected_failure;
    } catch (...) {
        result.error = configuration_error::unexpected_failure;
    }
    if (result.error != configuration_error::none) {
        result.config_json.clear();
        result.effective = {};
        try {
            result.diagnostics.push_back({.error = result.error, .field = std::string{stage}});
        } catch (const std::bad_alloc&) {
            result.error = configuration_error::allocation_failure;
        }
    }
    return result;
}
// NOLINTEND(llvm-prefer-static-over-anonymous-namespace,fuchsia-default-arguments-calls,altera-unroll-loops)
} // namespace

auto generate(const profile_payload::minimal_profile& material, const client_settings& settings,
              const runtime_context& context, std::span<const overrides::override_item> patches,
              const generation_limits& limits) noexcept -> configuration_result {
    return guarded(material, settings, context, patches, limits);
}
auto generate(const profile_payload::minimal_profile_snapshot& material,
              const client_settings& settings, const runtime_context& context,
              std::span<const overrides::override_item> patches,
              const generation_limits& limits) noexcept -> configuration_result {
    return guarded(material, settings, context, patches, limits);
}
} // namespace runtime_config
