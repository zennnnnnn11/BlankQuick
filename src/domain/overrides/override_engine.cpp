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
               const engine_options& options) -> void {
    if (std::ssize(patch.content) > options.max_patch_bytes) {
        detail::fail(override_error::resource_limit);
    }
    if (patch.ext == "yaml") {
        detail::merge_patch(candidate, detail::parse_yaml(patch.content, options), options);
    } else if (patch.ext == "js") {
        // Script execution belongs to the application pipeline. This library only
        // transforms data and never creates an interpreter, timer or worker thread.
        detail::fail(override_error::application_required);
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
    // 覆写按传入顺序消费前一步结果。先建立报告槽再执行，失败时最后一项
    // 就能记录出错的覆写 ID；不跳过失败项继续执行，避免生成来源不明的配置。
    for (const auto& patch : patches) {
        if (!patch.id.empty() && !detail::valid_identifier(patch.id)) {
            detail::fail(override_error::invalid_input);
        }
        reports.push_back({.id = patch.id});
        apply_one(candidate, patch, options);
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

auto read_document(std::string_view text, const engine_options& options) noexcept
    -> document_result {
    auto result = document_result{};
    try {
        check_options({}, options);
        result.value = detail::parse_json(text, options);
    } catch (const detail::override_failure& failure) {
        result.error = failure.code();
    } catch (const std::bad_alloc&) {
        result.error = override_error::allocation_failure;
    } catch (const std::exception&) {
        result.error = override_error::unexpected_failure;
    } catch (...) {
        result.error = override_error::unexpected_failure;
    }
    result.stage =
        result.error == override_error::none ? failure_stage::none : failure_stage::input;
    return result;
}

auto valid_identifier(std::string_view value) noexcept -> bool {
    return detail::valid_identifier(value);
}

auto valid_utf8(std::string_view value) noexcept -> bool {
    return detail::valid_utf8(value);
}
auto apply_document(document candidate, std::span<const override_item> patches,
                    const engine_options& options) noexcept -> document_result {
    // 按值接收候选，调用者可选择复制或移动。成功才把完整候选交回 value；
    // 失败只保留阶段和报告，不把内部改到一半的文档交给下一条处理链。
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
