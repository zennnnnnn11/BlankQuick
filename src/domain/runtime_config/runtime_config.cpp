#include <algorithm>
#include <blank/overrides/override_dom.hpp>
#include <blank/overrides/override_engine.hpp>
#include <blank/overrides/override_types.hpp>
#include <blank/profiles/minimal_profile.hpp>
#include <blank/runtime_config/configuration_document.hpp>
#include <blank/runtime_config/runtime_config.hpp>
#include <concepts>
#include <exception>
#include <functional>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "runtime_config_detail.hpp"

namespace runtime_config {
namespace {

// NOLINTBEGIN(llvm-prefer-static-over-anonymous-namespace,fuchsia-default-arguments-calls,altera-unroll-loops)
template <typename Material>
    requires std::same_as<Material, profile_payload::minimal_profile> ||
             std::same_as<Material, profile_payload::minimal_profile_snapshot>
auto compose(const Material& material, const client_settings& settings,
             const runtime_context& context, const generation_limits& limits,
             std::string_view& stage) -> detail::json {
    if (limits.max_nodes < 1 || limits.max_groups < 1 || limits.max_rules < 1 ||
        limits.max_references < 1 ||
        limits.patches.input_format != overrides::configuration_format::json) {
        detail::fail(configuration_error::invalid_settings);
    }
    // 客户端设置建立运行骨架，材料只补节点/组/规则等业务部分。
    // 这里不接受一整份原文直接覆盖骨架，避免绕过统一模型和客户端约束。
    stage = "settings";
    auto candidate = detail::base_document(settings, context);
    stage = "material";
    auto business = detail::export_material(material, limits);
    for (auto& [key, value] : business.items()) {
        const auto field = std::string{key};
        candidate.emplace(field, std::move(value));
    }
    return candidate;
}

auto complete(overrides::dom::document_result&& applied, const client_settings& settings,
              const runtime_context& context, const generation_limits& limits,
              configuration_result& result, std::string_view& stage) -> void {
    auto completed = std::move(applied);
    stage = "patches";
    result.patch_reports = std::move(completed.reports);
    if (completed.error == overrides::override_error::invalid_document &&
        completed.stage == overrides::dom::failure_stage::input) {
        detail::fail(configuration_error::invalid_document);
    }
    if (completed.error != overrides::override_error::none) {
        detail::fail(configuration_error::patch_failed);
    }
    // 整条覆写链结束后统一验证。中间草稿不是可运行配置，否则前一条
    // 覆写的暂态会阻止后一条覆写修正它。
    stage = "configuration";
    if (settings.dns.append_system_dns) {
        // “追加系统 DNS”属于客户端约束，在覆写后补入，并保持幂等。
        // 这里只写入内核识别的 system 标记，不在数据层查询操作系统的 DNS。
        auto& servers = completed.value.at("dns").at("nameserver");
        if (!servers.is_array()) {
            detail::fail(configuration_error::invalid_document);
        }
        if (std::ranges::none_of(servers, [](const detail::json& server) noexcept -> bool {
                const auto* const address = server.get_ptr<const std::string*>();
                return address != nullptr && (*address == "system" || *address == "system://");
            })) {
            servers.push_back("system");
        }
    }

    detail::validate_final(completed.value, context, limits, result.effective);
    result.config_json = completed.value.dump();
}

template <typename Operation>
    requires std::invocable<Operation, configuration_result&, std::string_view&>
[[nodiscard]] auto guarded(Operation&& operation) noexcept -> configuration_result {
    auto result = configuration_result{};
    auto stage = std::string_view{"limits"};
    try {
        std::invoke(std::forward<Operation>(operation), result, stage);
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
        // 错误值是唯一可供提交者判断的结果。清除可能在校验后半段才失败的
        // 输出，避免留下看起来可用但尚未完整验证的配置或生效设置。
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
template <typename Material>
    requires std::same_as<Material, profile_payload::minimal_profile> ||
             std::same_as<Material, profile_payload::minimal_profile_snapshot>
[[nodiscard]] auto draft(const Material& material, const client_settings& settings,
                         const runtime_context& context, const generation_limits& limits) noexcept
    -> configuration_draft {
    auto result = configuration_draft{};
    result.status = guarded([&](configuration_result&, std::string_view& stage) -> void {
        result.value = compose(material, settings, context, limits, stage);
    });
    return result;
}

template <typename Material>
    requires std::same_as<Material, profile_payload::minimal_profile> ||
             std::same_as<Material, profile_payload::minimal_profile_snapshot>
[[nodiscard]] auto generate_data(const Material& material, const client_settings& settings,
                                 const runtime_context& context,
                                 std::span<const overrides::override_item> patches,
                                 const generation_limits& limits) noexcept -> configuration_result {
    auto candidate = draft(material, settings, context, limits);
    if (candidate.status.error != configuration_error::none) {
        return std::move(candidate.status);
    }
    return finalize_configuration(
        overrides::dom::apply_document(std::move(candidate.value), patches, limits.patches),
        settings, context, limits);
}
// NOLINTEND(llvm-prefer-static-over-anonymous-namespace,fuchsia-default-arguments-calls,altera-unroll-loops)
} // namespace

auto make_draft(const profile_payload::minimal_profile& material, const client_settings& settings,
                const runtime_context& context, const generation_limits& limits) noexcept
    -> configuration_draft {
    return draft(material, settings, context, limits);
}

auto make_draft(const profile_payload::minimal_profile_snapshot& material,
                const client_settings& settings, const runtime_context& context,
                const generation_limits& limits) noexcept -> configuration_draft {
    return draft(material, settings, context, limits);
}

auto finalize_configuration(overrides::dom::document_result&& applied,
                            const client_settings& settings, const runtime_context& context,
                            const generation_limits& limits) noexcept -> configuration_result {
    return guarded([&](configuration_result& result, std::string_view& stage) -> void {
        complete(std::move(applied), settings, context, limits, result, stage);
    });
}

auto generate(const profile_payload::minimal_profile& material, const client_settings& settings,
              const runtime_context& context, std::span<const overrides::override_item> patches,
              const generation_limits& limits) noexcept -> configuration_result {
    return generate_data(material, settings, context, patches, limits);
}
auto generate(const profile_payload::minimal_profile_snapshot& material,
              const client_settings& settings, const runtime_context& context,
              std::span<const overrides::override_item> patches,
              const generation_limits& limits) noexcept -> configuration_result {
    return generate_data(material, settings, context, patches, limits);
}
} // namespace runtime_config
