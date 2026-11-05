#include <algorithm>
#include <array>
#include <blank/nodes/unified_node.hpp>
#include <blank/profiles/minimal_profile.hpp>
#include <blank/runtime_config/runtime_config.hpp>
#include <blank/shared/ordered_object.hpp>
#include <chrono>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "runtime_config_detail.hpp"

namespace runtime_config::detail {
namespace {

// NOLINTBEGIN(llvm-prefer-static-over-anonymous-namespace,altera-unroll-loops,fuchsia-default-arguments-calls,cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
constexpr std::int64_t maximum_port{65535};
template <typename Value> struct optional_type : std::false_type {};
template <typename Value> struct optional_type<std::optional<Value>> : std::true_type {};
template <typename Value> struct sequence_type : std::false_type {};
template <typename Value> struct sequence_type<std::vector<Value>> : std::true_type {};
template <typename Value, std::size_t Count>
struct sequence_type<std::array<Value, Count>> : std::true_type {};
template <typename Value> struct pair_type : std::false_type {};
template <typename First, typename Second>
struct pair_type<std::pair<First, Second>> : std::true_type {};
template <typename Value> struct duration_type : std::false_type {};
template <typename Rep, typename Period>
struct duration_type<std::chrono::duration<Rep, Period>> : std::true_type {};

template <typename Value>
concept export_value = std::is_object_v<Value>;

template <typename Value>
concept scalar_export =
    std::integral<Value> || std::same_as<Value, std::byte> || duration_type<Value>::value;

template <scalar_export Value> [[nodiscard]] auto encode(const Value& value) noexcept -> json {
    if constexpr (std::same_as<Value, std::byte>) {
        return std::to_integer<unsigned int>(value);
    } else if constexpr (duration_type<Value>::value) {
        return value.count();
    } else {
        return value;
    }
}

template <export_value Value>
    requires(!scalar_export<Value>)
[[nodiscard]] auto encode(const Value& value) -> json;

template <export_value Value>
    requires sequence_type<Value>::value
[[nodiscard]] auto encode_sequence(const Value& value) -> json {
    if constexpr (pair_type<typename Value::value_type>::value) {
        auto result = json::object();
        for (const auto& [key, item] : value) {
            if (!result.emplace(key, encode(item)).second) {
                fail(configuration_error::invalid_node);
            }
        }
        return result;
    } else {
        auto result = json::array();
        for (const auto& item : value) {
            result.push_back(encode(item));
        }
        return result;
    }
}

template <export_value Value>
    requires(!scalar_export<Value>)
[[nodiscard]] auto encode(const Value& value) -> json {
    if constexpr (optional_type<Value>::value) {
        return value ? encode(*value) : json(nullptr);
    } else if constexpr (std::same_as<Value, node_model::hopping_interval>) {
        return std::visit(
            []<export_value Alternative>(const Alternative& interval) noexcept(
                std::same_as<Alternative, std::monostate> ||
                duration_type<Alternative>::value) -> json {
                if constexpr (std::same_as<Alternative, std::monostate>) {
                    return nullptr;
                } else {
                    return encode(interval);
                }
            },
            value);
    } else if constexpr (std::same_as<Value, node_model::ip_preference>) {
        switch (value) {
        case node_model::ip_preference::dual:
            return "dual";
        case node_model::ip_preference::ipv4:
            return "ipv4";
        case node_model::ip_preference::ipv6:
            return "ipv6";
        case node_model::ip_preference::ipv4_prefer:
            return "ipv4-prefer";
        case node_model::ip_preference::ipv6_prefer:
            return "ipv6-prefer";
        }
        fail(configuration_error::invalid_node);
    } else if constexpr (sequence_type<Value>::value) {
        return encode_sequence(value);
    } else if constexpr (std::floating_point<Value>) {
        if (!std::isfinite(value)) {
            fail(configuration_error::invalid_node);
        }
        return value;
    } else {
        return json(value);
    }
}

template <export_value Value> [[nodiscard]] auto is_default(const Value& value) noexcept -> bool {
    if constexpr (optional_type<Value>::value) {
        return !value;
    } else if constexpr (std::same_as<Value, node_model::hopping_interval>) {
        return std::holds_alternative<std::monostate>(value);
    } else if constexpr (requires { value.empty(); }) {
        return value.empty();
    } else if constexpr (duration_type<Value>::value) {
        return value.count() == 0;
    } else if constexpr (std::is_enum_v<Value> || std::is_arithmetic_v<Value>) {
        return value == Value{};
    } else {
        return false;
    }
}

template <export_value Value>
[[nodiscard]] constexpr auto has_negative(const Value& value) noexcept -> bool {
    if constexpr (std::signed_integral<Value>) {
        return value < 0;
    } else if constexpr (duration_type<Value>::value) {
        return value.count() < 0;
    } else if constexpr (sequence_type<Value>::value) {
        return std::ranges::any_of(value, []<export_value Item>(const Item& item) noexcept -> bool {
            return has_negative(item);
        });
    } else {
        return false;
    }
}

template <export_value Value>
auto write_field(json& target, std::string_view key, const Value& value, bool required,
                 bool unsigned_input) -> void {
    if constexpr (optional_type<Value>::value) {
        if (value) {
            write_field(target, key, *value, true, unsigned_input);
        }
    } else {
        if (unsigned_input && has_negative(value)) {
            fail(configuration_error::invalid_node);
        }
        if (required || !is_default(value)) {
            target[std::string{key}] = encode(value);
        }
    }
}

auto emit_object(json& target, std::string_view key, json value, bool required) -> void {
    if (required || !value.empty()) {
        target[std::string{key}] = std::move(value);
    }
}

template <export_value Value, typename Operation>
    requires std::invocable<Operation, const Value&>
auto emit_optional_object(json& target, std::string_view key, const std::optional<Value>& value,
                          Operation operation) -> void {
    if (value) {
        emit_object(target, key, operation(*value), true);
    }
}

template <typename Value, typename Operation>
    requires std::invocable<Operation, const Value&>
auto emit_array(json& target, std::string_view key, const std::vector<Value>& values,
                Operation operation, bool required) -> void {
    if (required || !values.empty()) {
        auto result = json::array();
        for (const auto& value : values) {
            result.push_back(operation(value));
        }
        target[std::string{key}] = std::move(result);
    }
}

template <typename Value> struct variant_name_tag;
template <> struct variant_name_tag<node_model::websocket_transport> {
    static constexpr std::string_view name{"ws"};
};
template <> struct variant_name_tag<node_model::grpc_transport> {
    static constexpr std::string_view name{"grpc"};
};
template <> struct variant_name_tag<node_model::http_transport> {
    static constexpr std::string_view name{"http"};
};
template <> struct variant_name_tag<node_model::http2_transport> {
    static constexpr std::string_view name{"h2"};
};
template <> struct variant_name_tag<node_model::xhttp_transport> {
    static constexpr std::string_view name{"xhttp"};
};
template <> struct variant_name_tag<node_model::mkcp_transport> {
    static constexpr std::string_view name{"kcp"};
};
template <> struct variant_name_tag<node_model::mekya_transport> {
    static constexpr std::string_view name{"mekya"};
};
template <> struct variant_name_tag<node_model::simple_obfs_param> {
    static constexpr std::string_view name{"obfs"};
};
template <> struct variant_name_tag<node_model::v2ray_plugin_param> {
    static constexpr std::string_view name{"v2ray-plugin"};
};
template <> struct variant_name_tag<node_model::gost_plugin_param> {
    static constexpr std::string_view name{"gost-plugin"};
};
template <> struct variant_name_tag<node_model::shadow_tls_plugin_param> {
    static constexpr std::string_view name{"shadow-tls"};
};
template <> struct variant_name_tag<node_model::restls_plugin_param> {
    static constexpr std::string_view name{"restls"};
};
template <> struct variant_name_tag<node_model::jls_plugin_param> {
    static constexpr std::string_view name{"jls"};
};
template <> struct variant_name_tag<node_model::kcptun_plugin_param> {
    static constexpr std::string_view name{"kcptun"};
};

template <typename... Values>
[[nodiscard]] auto variant_name(const std::variant<Values...>& value) -> std::string_view {
    if (value.valueless_by_exception()) {
        fail(configuration_error::invalid_node);
    }
    return std::visit(
        []<export_value Alternative>(const Alternative&) -> std::string_view {
            if constexpr (std::same_as<Alternative, std::monostate>) {
                return {};
            } else {
                return variant_name_tag<Alternative>::name;
            }
        },
        value);
}

// NOLINTNEXTLINE(misc-no-recursion)
auto merge_export(json& target, const json& source) -> void {
    for (const auto& [key, value] : source.items()) {
        if (target.contains(key) && target[key].is_object() && value.is_object()) {
            merge_export(target[key], value);
        } else if (target.contains(key) && target[key] != value) {
            fail(configuration_error::invalid_node);
        } else {
            target[key] = value;
        }
    }
}

#include "mihomo_export_fields.inc"

[[nodiscard]] auto group_name(profile_payload::group_type type) -> std::string_view {
    switch (type) {
    case profile_payload::group_type::select:
        return "select";
    case profile_payload::group_type::url_test:
        return "url-test";
    case profile_payload::group_type::fallback:
        return "fallback";
    case profile_payload::group_type::load_balance:
        return "load-balance";
    }
    fail(configuration_error::invalid_document);
}

[[nodiscard]] auto export_group(const profile_payload::proxy_group& group) -> json {
    auto result = json{{"name", group.name}, {"type", group_name(group.type)}};
    write_field(result, "proxies", group.proxies, false, false);
    write_field(result, "url", group.url, false, false);
    write_field(result, "interval", group.interval, false, true);
    write_field(result, "timeout", group.timeout, false, true);
    write_field(result, "max-failed-times", group.max_failed_times, false, true);
    write_field(result, "empty-fallback", group.empty_fallback, false, false);
    write_field(result, "lazy", group.lazy, false, false);
    write_field(result, "disable-udp", group.disable_udp, false, false);
    write_field(result, "filter", group.filter, false, false);
    write_field(result, "exclude-filter", group.exclude_filter, false, false);
    write_field(result, "exclude-type", group.exclude_type, false, false);
    write_field(result, "expected-status", group.expected_status, false, false);
    write_field(result, "include-all-proxies", group.include_all_proxies, false, false);
    write_field(result, "hidden", group.hidden, false, false);
    write_field(result, "icon", group.icon, false, false);
    write_field(result, "default-selected", group.default_selected, false, false);
    write_field(result, "tolerance", group.tolerance, false, true);
    write_field(result, "strategy", group.strategy, false, false);
    write_field(result, "hash-key", group.hash_key, false, false);
    return result;
}

[[nodiscard]] auto export_provider(const profile_payload::rule_provider& provider) -> json {
    auto result = json::object();
    switch (provider.type) {
    case profile_payload::provider_type::http:
        result["type"] = "http";
        break;
    case profile_payload::provider_type::file:
        result["type"] = "file";
        break;
    case profile_payload::provider_type::inline_payload:
        result["type"] = "inline";
        break;
    }
    switch (provider.behavior) {
    case profile_payload::rule_behavior::domain:
        result["behavior"] = "domain";
        break;
    case profile_payload::rule_behavior::ipcidr:
        result["behavior"] = "ipcidr";
        break;
    case profile_payload::rule_behavior::classical:
        result["behavior"] = "classical";
        break;
    }
    switch (provider.format) {
    case profile_payload::rule_format::yaml:
        result["format"] = "yaml";
        break;
    case profile_payload::rule_format::text:
        result["format"] = "text";
        break;
    case profile_payload::rule_format::mrs:
        result["format"] = "mrs";
        break;
    }
    if (result.size() != 3) {
        fail(configuration_error::invalid_document);
    }
    write_field(result, "path", provider.path, false, false);
    write_field(result, "url", provider.url, false, false);
    write_field(result, "proxy", provider.proxy, false, false);
    write_field(result, "interval", provider.interval, false, true);
    write_field(result, "size-limit", provider.size_limit, false, true);
    write_field(result, "payload", provider.payload,
                provider.type == profile_payload::provider_type::inline_payload, false);
    write_field(result, "header", provider.header, false, false);
    write_field(result, "path-in-bundle", provider.path_in_bundle, false, false);
    return result;
}

template <typename Material>
    requires std::same_as<Material, profile_payload::minimal_profile> ||
             std::same_as<Material, profile_payload::minimal_profile_snapshot>
[[nodiscard]] auto material_document(const Material& material, const generation_limits& limits)
    -> json {
    if (std::ssize(material.proxies) > limits.max_nodes ||
        std::ssize(material.proxy_groups) > limits.max_groups ||
        std::ssize(material.rules) > limits.max_rules ||
        std::ssize(material.rule_providers) > limits.max_groups) {
        fail(configuration_error::resource_limit);
    }
    auto result = json{
        {"proxies", json::array()},
        {"proxy-groups", json::array()},
        {"rules", material.rules},
        {"rule-providers", json::object()},
    };
    result.at("proxies").get_ref<json::array_t&>().reserve(material.proxies.size());
    result.at("proxy-groups").get_ref<json::array_t&>().reserve(material.proxy_groups.size());
    for (const auto& node : material.proxies) {
        if constexpr (std::same_as<Material, profile_payload::minimal_profile>) {
            result["proxies"].push_back(export_node(node));
        } else {
            if (!node) {
                fail(configuration_error::invalid_node);
            }
            result["proxies"].push_back(export_node(*node));
        }
    }
    for (const auto& group : material.proxy_groups) {
        result["proxy-groups"].push_back(export_group(group));
    }
    auto providers = json_detail::ordered_object{result.at("rule-providers")};
    for (const auto& provider : material.rule_providers) {
        if (provider.name.empty() || providers.contains(provider.name)) {
            fail(configuration_error::invalid_document);
        }
        providers.member(provider.name) = export_provider(provider);
    }
    return result;
}
// NOLINTEND(llvm-prefer-static-over-anonymous-namespace,altera-unroll-loops,fuchsia-default-arguments-calls,cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
} // namespace

auto export_node(const node_model::unified_node& node) -> json {
    // 统一模型不等于任意 JSON：协议枚举必须与 variant 中的具体参数类型匹配。
    // 生成字段清单负责映射字段名，手写边界负责拒绝不一致的模型和无效端点。
    if (node.core.name.empty() || !node_model::has_matching_protocol(node)) {
        fail(configuration_error::invalid_node);
    }
    if (node.core.remote && (node.core.remote->host.empty() || node.core.remote->port < 0 ||
                             node.core.remote->port > maximum_port)) {
        fail(configuration_error::invalid_node);
    }
    auto result = export_node_fields(node);
    if (result.contains("server") && result.at("server").get_ref<const std::string&>().empty()) {
        fail(configuration_error::invalid_node);
    }
    return result;
}

auto export_material(const profile_payload::minimal_profile& material,
                     const generation_limits& limits) -> json {
    return material_document(material, limits);
}
auto export_material(const profile_payload::minimal_profile_snapshot& material,
                     const generation_limits& limits) -> json {
    return material_document(material, limits);
}
} // namespace runtime_config::detail
