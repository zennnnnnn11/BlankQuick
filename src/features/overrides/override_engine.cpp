#include <blank/overrides/override_dom.hpp>
#include <blank/overrides/override_engine.hpp>
#include <blank/overrides/override_types.hpp>
#include <exception>
#include <new>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "override_detail.hpp"

namespace overrides {
namespace {
// NOLINTBEGIN(llvm-prefer-static-over-anonymous-namespace,altera-unroll-loops,fuchsia-default-arguments-calls)
auto apply_one(detail::document& candidate, const override_item& patch,
               const engine_options& options, patch_report& report) -> void {
    if (std::ssize(patch.content) > options.max_patch_bytes) {
        detail::fail(override_error::resource_limit);
    }
    if (patch.ext == "yaml") {
        detail::merge_patch(candidate, detail::parse_yaml(patch.content, options), options);
    } else if (patch.ext == "js") {
        candidate = detail::run_script(candidate, patch, options, report);
    } else {
        detail::fail(override_error::invalid_input);
    }
}

auto check_options(std::span<const override_item> patches, const engine_options& options) -> void {
    if (!detail::valid_options(options)) {
        detail::fail(override_error::invalid_input);
    }
    if (std::ssize(patches) > options.max_patches) {
        detail::fail(override_error::resource_limit);
    }
}

auto apply_chain(detail::document& candidate, std::span<const override_item> patches,
                 const engine_options& options, std::vector<patch_report>& reports) -> void {
    for (const auto& patch : patches) {
        if (!patch.id.empty() && !detail::valid_identifier(patch.id)) {
            detail::fail(override_error::invalid_input);
        }
        reports.push_back({.id = patch.id});
        apply_one(candidate, patch, options, reports.back());
    }
}
auto apply_text(std::string_view configuration, std::span<const override_item> patches,
                const engine_options& options, apply_result& result) -> void {
    check_options(patches, options);
    auto candidate = options.input_format == configuration_format::yaml
                         ? detail::parse_yaml(configuration, options)
                         : detail::parse_json(configuration, options);
    apply_chain(candidate, patches, options, result.reports);
    result.config_json = candidate.dump();
}
// NOLINTEND(llvm-prefer-static-over-anonymous-namespace,altera-unroll-loops,fuchsia-default-arguments-calls)
} // namespace

namespace dom {
auto apply_document(document candidate, std::span<const override_item> patches,
                    const engine_options& options) noexcept -> document_result {
    auto result = document_result{};
    result.stage = failure_stage::input;
    try {
        check_options(patches, options);
        detail::validate_document(candidate, options);
        result.stage = failure_stage::patch;
        apply_chain(candidate, patches, options, result.reports);
        result.value = std::move(candidate);
        result.stage = failure_stage::none;
    } catch (const detail::override_failure& failure) {
        result.error = failure.code();
    } catch (const document::exception&) {
        result.error = override_error::invalid_document;
    } catch (const std::bad_alloc&) {
        result.error = override_error::allocation_failure;
    } catch (const std::exception&) {
        result.error = override_error::unexpected_failure;
    } catch (...) {
        result.error = override_error::unexpected_failure;
    }
    if (result.error != override_error::none && !result.reports.empty()) {
        result.reports.back().error = result.error;
    }
    return result;
}
} // namespace dom

auto apply_overrides(std::string_view configuration, std::span<const override_item> patches,
                     const engine_options& options) noexcept -> apply_result {
    auto result = apply_result{};
    try {
        apply_text(configuration, patches, options, result);
    } catch (const detail::override_failure& failure) {
        result.error = failure.code();
    } catch (const std::bad_alloc&) {
        result.error = override_error::allocation_failure;
    } catch (const std::exception&) {
        result.error = override_error::unexpected_failure;
    } catch (...) {
        result.error = override_error::unexpected_failure;
    }
    if (result.error != override_error::none && !result.reports.empty()) {
        result.reports.back().error = result.error;
    }
    return result;
}

} // namespace overrides
