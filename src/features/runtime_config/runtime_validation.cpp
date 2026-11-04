#include <QtCore/QString>
#include <QtCore/QUrl>
#include <QtNetwork/QHostAddress>

#include <algorithm>
#include <array>
#include <blank/runtime_config/runtime_config.hpp>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iterator>
#include <limits>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "runtime_config_detail.hpp"

namespace runtime_config::detail {
namespace {
// CPU loops and explicit aggregate initialization follow repository policy.
// NOLINTBEGIN(llvm-prefer-static-over-anonymous-namespace,altera-struct-pack-align,fuchsia-default-arguments-calls,altera-unroll-loops,readability-redundant-member-init)
constexpr std::int64_t maximum_port{65535};
struct schema_field {
    std::string_view path{};
    std::string_view type{};
};
#include "runtime_schema.inc"

[[nodiscard]] auto text(const json& value) -> const std::string& {
    if (!value.is_string()) {
        fail(configuration_error::invalid_document);
    }
    return value.get_ref<const std::string&>();
}

[[nodiscard]] auto number(const json& value) -> std::int64_t {
    if (!value.is_number_integer() ||
        (value.is_number_unsigned() &&
         value.get<std::uint64_t>() >
             static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))) {
        fail(configuration_error::invalid_document);
    }
    return value.get<std::int64_t>();
}

[[nodiscard]] auto enabled(const json& value, std::string_view key) -> bool {
    const auto found = value.find(key);
    if (found == value.end()) {
        return false;
    }
    if (!found->is_boolean()) {
        fail(configuration_error::invalid_document);
    }
    return found->get<bool>();
}

[[nodiscard]] auto string_field(const json& value, std::string_view key) -> std::string_view {
    const auto found = value.find(key);
    return found == value.end() ? std::string_view{} : std::string_view{text(*found)};
}

[[nodiscard]] auto collection(const json& value, std::string_view key, bool object) -> const json& {
    const auto found = value.find(key);
    if (found == value.end() || (object ? !found->is_object() : !found->is_array())) {
        fail(configuration_error::invalid_document);
    }
    return *found;
}

auto validate_fixed_object(const json& value, std::string_view prefix) -> void;

[[nodiscard]] constexpr auto string_type(std::string_view type) noexcept -> bool {
    return type == "string" || type.starts_with("netip.") || type == "T.TunnelMode" ||
           type == "log.LogLevel" || type == "process.FindProcessMode" || type == "C.DNSMode" ||
           type == "C.FilterMode" || type == "C.TUNStack";
}

auto fixed_integer(const json& value, std::string_view type) -> void {
    const auto integer = number(value);
    if ((type.starts_with("uint") && integer < 0) ||
        (type == "uint32" &&
         std::cmp_greater(integer, std::numeric_limits<std::uint32_t>::max()))) {
        fail(configuration_error::invalid_document);
    }
}

// The patch parser bounds depth before this schema walk; only fixed schema objects recurse.
// NOLINTNEXTLINE(misc-no-recursion)
auto validate_fixed_value(const json& value, std::string_view path, std::string_view type) -> void {
    if (value.is_null()) {
        fail(configuration_error::invalid_document);
    }
    if (type.starts_with('*')) {
        type.remove_prefix(1);
    }
    // ISO C++20 intentionally does not depend on Abseil.
    // NOLINTNEXTLINE(abseil-string-find-str-contains)
    if (type.starts_with("map[") || type.find("orderedmap.") != std::string_view::npos) {
        if (!value.is_object()) {
            fail(configuration_error::invalid_document);
        }
    } else if (type.starts_with("[]")) {
        if (!value.is_array()) {
            fail(configuration_error::invalid_document);
        }
        for (const auto& item : value) {
            if (item.is_string() && path == "tunnels") {
                continue;
            }
            validate_fixed_value(item, std::string{path} + "[]", type.substr(2));
        }
    } else if (type == "bool") {
        if (!value.is_boolean()) {
            fail(configuration_error::invalid_document);
        }
    } else if (type.starts_with("int") || type.starts_with("uint")) {
        fixed_integer(value, type);
    } else if (string_type(type)) {
        static_cast<void>(text(value));
    } else if (value.is_object()) {
        validate_fixed_object(value, std::string{path} + '.');
    } else {
        fail(configuration_error::invalid_document);
    }
}

// NOLINTNEXTLINE(misc-no-recursion)
auto validate_fixed_object(const json& value, std::string_view prefix) -> void {
    for (const auto& [key, item] : value.items()) {
        const auto path = std::string{prefix} + key;
        const auto found = std::ranges::find(input_schema, path, &schema_field::path);
        if (found == input_schema.end()) {
            fail(configuration_error::invalid_document);
        }
        validate_fixed_value(item, path, found->type);
    }
}

constexpr auto builtins = std::to_array<std::string_view>(
    {"DIRECT", "REJECT", "REJECT-DROP", "COMPATIBLE", "PASS", "PASS-RULE", "GLOBAL"});
[[nodiscard]] auto builtin(std::string_view name) noexcept -> bool {
    return std::ranges::find(builtins, name) != builtins.end();
}

using name_set = std::unordered_set<std::string_view>;

enum class target_kind : std::int8_t { node, group, subrule };
struct graph_target {
    std::size_t index{};
    target_kind kind{};
};
// MSVC STL debug-iterator allocation is diagnosed despite noexcept constructors.
struct reference_graph { // NOLINT(bugprone-exception-escape)
    std::unordered_map<std::string_view, graph_target> names{};
    std::vector<std::vector<std::size_t>> edges{};
    std::vector<std::int64_t> incoming{};
    std::int64_t count{};
};

auto insert_name(reference_graph& graph, const json& entry, target_kind kind) -> void {
    const auto name = string_field(entry, "name");
    const auto global_group = kind == target_kind::group && name == "GLOBAL";
    if (name.empty() || (builtin(name) && !global_group) ||
        !graph.names.emplace(name, graph_target{.index = graph.names.size(), .kind = kind})
             .second) {
        fail(configuration_error::invalid_reference);
    }
}

auto require_target(const reference_graph& graph, std::string_view target) -> void {
    if (!builtin(target) && !graph.names.contains(target)) {
        fail(configuration_error::invalid_reference);
    }
}

// Source/target order is the conventional directed edge notation; all calls are private.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
auto add_edge(reference_graph& graph, std::string_view source, std::string_view target,
              std::int64_t limit) -> void {
    if (++graph.count > limit) {
        fail(configuration_error::resource_limit);
    }
    require_target(graph, target);
    if (const auto found = graph.names.find(target); found != graph.names.end()) {
        graph.edges.at(graph.names.at(source).index).push_back(found->second.index);
        ++graph.incoming.at(found->second.index);
    }
}

auto check_cycles(reference_graph& graph) -> void {
    auto queue = std::vector<std::size_t>{};
    const auto indices = std::views::iota(std::size_t{}, graph.incoming.size());
    std::ranges::copy_if(indices, std::back_inserter(queue), [&graph](std::size_t index) -> bool {
        return graph.incoming.at(index) == 0;
    });
    for (auto head = std::size_t{}; head < queue.size(); ++head) {
        std::ranges::copy_if(
            graph.edges.at(queue.at(head)), std::back_inserter(queue),
            [&graph](std::size_t target) -> bool { return --graph.incoming.at(target) == 0; });
    }
    if (queue.size() != graph.incoming.size()) {
        fail(configuration_error::circular_reference);
    }
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
auto group_fallback(const json& group, const reference_graph& graph) -> void {
    const auto fallback = string_field(group, "empty-fallback");
    if (fallback.empty()) {
        return;
    }
    require_target(graph, fallback);
    if ((!builtin(fallback) && graph.names.at(fallback).kind != target_kind::node) ||
        fallback == "GLOBAL") {
        fail(configuration_error::invalid_reference);
    }
}

// Group and collection arguments use JSON privately, with fixed named call sites.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
auto group_edges(reference_graph& graph, const json& group, const json& nodes,
                 const name_set& providers, const generation_limits& limits) -> void {
    const auto name = string_field(group, "name");
    const auto type = string_field(group, "type");
    if (type != "select" && type != "url-test" && type != "fallback" && type != "load-balance") {
        fail(configuration_error::invalid_document);
    }
    auto has_members = false;
    if (const auto found = group.find("proxies"); found != group.end()) {
        if (!found->is_array()) {
            fail(configuration_error::invalid_document);
        }
        for (const auto& target : *found) {
            add_edge(graph, name, text(target), limits.max_references);
        }
        has_members = !found->empty();
    }
    if (enabled(group, "include-all") || enabled(group, "include-all-proxies")) {
        for (const auto& node : nodes) {
            add_edge(graph, name, string_field(node, "name"), limits.max_references);
        }
        has_members = true;
    }
    if (enabled(group, "include-all") || enabled(group, "include-all-providers")) {
        has_members = true;
    }
    if (const auto found = group.find("use"); found != group.end()) {
        if (!found->is_array()) {
            fail(configuration_error::invalid_document);
        }
        for (const auto& provider : *found) {
            if (!providers.contains(text(provider))) {
                fail(configuration_error::invalid_reference);
            }
        }
        has_members = has_members || !found->empty();
    }
    if (!has_members) {
        fail(configuration_error::invalid_reference);
    }
    group_fallback(group, graph);
    if (type == "load-balance" && string_field(group, "strategy") == "round-robin" &&
        !string_field(group, "hash-key").empty()) {
        fail(configuration_error::invalid_document);
    }
}

[[nodiscard]] auto index_names(const json& object) -> name_set {
    auto names = name_set{};
    names.reserve(object.size());
    for (const auto& [key, value] : object.items()) {
        static_cast<void>(value);
        names.insert(key);
    }
    return names;
}

[[nodiscard]] auto make_subrule_graph(const json& subrules) -> reference_graph {
    auto graph = reference_graph{};
    graph.names.reserve(subrules.size());
    for (const auto& [name, rules] : subrules.items()) {
        static_cast<void>(rules);
        if (name.empty()) {
            fail(configuration_error::invalid_reference);
        }
        graph.names.emplace(
            name, graph_target{.index = graph.names.size(), .kind = target_kind::subrule});
    }
    graph.edges.resize(graph.names.size());
    graph.incoming.resize(graph.names.size());
    return graph;
}

// A synchronous borrowed validation scope; all referenced documents outlive it.
// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
struct rule_context {
    rule_context(const name_set& input_providers, const reference_graph& input_outbounds,
                 reference_graph& input_subrules, const generation_limits& input_limits) noexcept
        : providers{input_providers}, outbounds{input_outbounds}, subrules{input_subrules},
          limits{input_limits} {}
    std::reference_wrapper<const name_set> providers;
    std::reference_wrapper<const reference_graph> outbounds;
    std::reference_wrapper<reference_graph> subrules;
    std::reference_wrapper<const generation_limits> limits;
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

[[nodiscard]] auto upper_rule_type(std::string_view value) -> std::string {
    auto result = std::string{value};
    std::ranges::transform(result, result.begin(), [](char character) noexcept -> char {
        return character >= 'a' && character <= 'z' ? static_cast<char>(character - 'a' + 'A')
                                                    : character;
    });
    return result;
}

[[nodiscard]] auto split_rule(std::string_view rule) -> std::vector<std::string_view> {
    auto result = std::vector<std::string_view>{};
    while (true) {
        const auto comma = rule.find(',');
        auto part = rule.substr(0, comma);
        const auto first = part.find_first_not_of(" \t\r\n");
        part = first == std::string_view::npos
                   ? std::string_view{}
                   : part.substr(first, part.find_last_not_of(" \t\r\n") - first + 1);
        result.push_back(part);
        if (comma == std::string_view::npos) {
            return result;
        }
        rule.remove_prefix(comma + 1);
    }
}

// Source and target follow the same directed-edge convention as add_edge.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
auto add_subrule_reference(std::string_view source, std::string_view target,
                           const rule_context& context) -> void {
    auto& subrules = context.subrules.get();
    if (!subrules.names.contains(target)) {
        fail(configuration_error::invalid_reference);
    }
    const auto remaining = context.limits.get().max_references - context.outbounds.get().count;
    if (source.empty()) {
        if (++subrules.count > remaining) {
            fail(configuration_error::resource_limit);
        }
    } else {
        add_edge(subrules, source, target, remaining);
    }
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
auto rule_references(const json& rules, std::string_view source, const rule_context& context)
    -> void {
    const auto& limits = context.limits.get();
    const auto& providers = context.providers.get();
    const auto& graph = context.outbounds.get();
    if (!rules.is_array() || std::ssize(rules) > limits.max_rules) {
        fail(configuration_error::resource_limit);
    }
    constexpr auto last_target = std::to_array<std::string_view>(
        {"AND", "OR", "NOT", "DOMAIN-REGEX", "PROCESS-NAME-REGEX", "PROCESS-PATH-REGEX"});
    for (const auto& rule : rules) {
        const auto parts = split_rule(text(rule));
        if (parts.size() < 2 || parts.front().empty()) {
            fail(configuration_error::invalid_document);
        }
        const auto type = upper_rule_type(parts.front());
        if (type == "SUB-RULE") {
            if (parts.size() < 3) {
                fail(configuration_error::invalid_reference);
            }
            add_subrule_reference(source, parts.back(), context);
            continue;
        }
        const auto last = std::ranges::find(last_target, type) != last_target.end();
        const auto index = type == "MATCH" ? std::size_t{1} : std::size_t{2};
        if (parts.size() <= index) {
            fail(configuration_error::invalid_document);
        }
        require_target(graph, last ? parts.back() : parts.at(index));
        if (type == "RULE-SET" && !providers.contains(parts.at(1))) {
            fail(configuration_error::invalid_reference);
        }
    }
}

auto safe_provider_path(std::string_view value, const runtime_context& context) -> void {
    if (value.empty()) {
        return;
    }
    // NOLINTNEXTLINE(abseil-string-find-str-contains)
    if (value.find('\0') != std::string_view::npos) {
        fail(configuration_error::invalid_path);
    }
    const auto path =
        std::filesystem::path{QString::fromStdString(std::string{value}).toStdWString()};
    const auto root = context.data_directory.lexically_normal();
    const auto resolved = (path.is_absolute() ? path : root / path).lexically_normal();
    const auto relative = resolved.lexically_relative(root);
    if (relative.empty() ||
        std::ranges::find(relative, std::filesystem::path{".."}) != relative.end()) {
        fail(configuration_error::invalid_path);
    }
}

auto provider_checks(const json& providers, const reference_graph& graph,
                     const runtime_context& context) -> void {
    for (const auto& [name, provider] : providers.items()) {
        if (name.empty() || !provider.is_object()) {
            fail(configuration_error::invalid_document);
        }
        const auto proxy = string_field(provider, "proxy");
        if (!proxy.empty()) {
            require_target(graph, proxy);
        }
        safe_provider_path(string_field(provider, "path"), context);
        const auto type = string_field(provider, "type");
        if (type != "http" && type != "file" && type != "inline") {
            fail(configuration_error::invalid_document);
        }
        if (type == "file" && string_field(provider, "path").empty()) {
            fail(configuration_error::invalid_path);
        }
        if (type == "http") {
            const auto address =
                QUrl{QString::fromStdString(std::string{string_field(provider, "url")}),
                     QUrl::StrictMode};
            if (!address.isValid() || address.host().isEmpty() ||
                (address.scheme() != QStringLiteral("https") &&
                 address.scheme() != QStringLiteral("http"))) {
                fail(configuration_error::invalid_document);
            }
        }
    }
}

auto bootstrap_checks(const json& dns) -> void {
    if (const auto boot = dns.find("default-nameserver"); boot != dns.end()) {
        if (boot->empty()) {
            fail(configuration_error::invalid_document);
        }
        for (const auto& item : *boot) {
            const auto& server = text(item);
            if (server == "system") {
                continue;
            }
            auto host = QString::fromStdString(server);
            // NOLINTNEXTLINE(abseil-string-find-str-contains)
            if (server.find("://") != std::string::npos) {
                host = QUrl{host}.host();
            } else if (QHostAddress{host}.isNull()) {
                host = QUrl{QStringLiteral("udp://") + host}.host();
            }
            if (QHostAddress{host}.isNull()) {
                fail(configuration_error::invalid_document);
            }
        }
    }
}

auto dns_engine_checks(const json& dns) -> void {
    const auto mode = string_field(dns, "enhanced-mode");
    if (!enabled(dns, "enable") || (mode != "fake-ip" && mode != "redir-host")) {
        fail(configuration_error::invalid_document);
    }
    const auto found = dns.find("nameserver");
    if (found == dns.end() || found->empty()) {
        fail(configuration_error::invalid_document);
    }
}

auto dns_checks(const json& document, const name_set& providers) -> void {
    const auto& dns = collection(document, "dns", true);
    dns_engine_checks(dns);
    const auto proxy = dns.find("proxy-server-nameserver");
    if (enabled(dns, "respect-rules") && (proxy == dns.end() || proxy->empty())) {
        fail(configuration_error::invalid_document);
    }
    if (dns.contains("proxy-server-nameserver-policy") &&
        !dns.at("proxy-server-nameserver-policy").empty() &&
        (proxy == dns.end() || proxy->empty())) {
        fail(configuration_error::invalid_document);
    }
    bootstrap_checks(dns);
    if (const auto policy = dns.find("nameserver-policy"); policy != dns.end()) {
        for (const auto& [key, value] : policy->items()) {
            static_cast<void>(value);
            constexpr std::string_view prefix{"rule-set:"};
            if (key.starts_with(prefix)) {
                const auto names = split_rule(std::string_view{key}.substr(prefix.size()));
                for (const auto name : names) {
                    if (!providers.contains(name)) {
                        fail(configuration_error::invalid_reference);
                    }
                }
            }
        }
    }
}

auto protected_checks(const json& document, const runtime_context& context) -> void {
    if (string_field(document, "external-controller") != context.controller ||
        string_field(document, "secret") != context.secret) {
        fail(configuration_error::protected_field);
    }
    constexpr auto forbidden = std::to_array<std::string_view>({
        "external-controller-tls",
        "external-controller-unix",
        "external-controller-pipe",
        "external-doh-server",
    });
    for (const auto key : forbidden) {
        if (!string_field(document, key).empty()) {
            fail(configuration_error::protected_field);
        }
    }
    const auto& cors = collection(document, "external-controller-cors", true);
    if (enabled(cors, "allow-private-network") || !cors.contains("allow-origins") ||
        cors.at("allow-origins") != json::array({"http://" + context.controller})) {
        fail(configuration_error::protected_field);
    }
}
} // namespace

auto validate_final(const json& document, const runtime_context& context,
                    const generation_limits& limits, effective_configuration& effective) -> void {
    if (!document.is_object()) {
        fail(configuration_error::invalid_document);
    }
    validate_fixed_object(document, "");
    protected_checks(document, context);
    const auto& nodes = collection(document, "proxies", false);
    const auto& groups = collection(document, "proxy-groups", false);
    const auto& rules = collection(document, "rules", false);
    const auto& providers = collection(document, "rule-providers", true);
    const auto empty = json::object();
    const auto& proxy_providers =
        document.contains("proxy-providers") ? document.at("proxy-providers") : empty;
    const auto& subrules = document.contains("sub-rules") ? document.at("sub-rules") : empty;
    if (std::ssize(nodes) > limits.max_nodes || std::ssize(groups) > limits.max_groups ||
        std::ssize(providers) > limits.max_groups) {
        fail(configuration_error::resource_limit);
    }
    const auto provider_names = index_names(providers);
    const auto proxy_provider_names = index_names(proxy_providers);
    auto graph = reference_graph{};
    graph.names.reserve(nodes.size() + groups.size());
    // Target kinds make validation independent of registration order.
    for (const auto& group : groups) {
        insert_name(graph, group, target_kind::group);
    }
    for (const auto& node : nodes) {
        insert_name(graph, node, target_kind::node);
    }
    graph.edges.resize(graph.names.size());
    graph.incoming.resize(graph.names.size());
    for (const auto& node : nodes) {
        const auto dialer = string_field(node, "dialer-proxy");
        if (!dialer.empty()) {
            add_edge(graph, string_field(node, "name"), dialer, limits.max_references);
        }
    }
    for (const auto& group : groups) {
        group_edges(graph, group, nodes, proxy_provider_names, limits);
    }
    check_cycles(graph);
    provider_checks(providers, graph, context);
    provider_checks(proxy_providers, graph, context);
    auto subgraph = make_subrule_graph(subrules);
    const auto rule_scope = rule_context{provider_names, graph, subgraph, limits};
    rule_references(rules, "", rule_scope);
    for (const auto& [name, item] : subrules.items()) {
        rule_references(item, name, rule_scope);
    }
    check_cycles(subgraph);
    dns_checks(document, provider_names);
    effective.mixed_port = number(document.at("mixed-port"));
    if (effective.mixed_port < 1 || effective.mixed_port > maximum_port) {
        fail(configuration_error::invalid_document);
    }
    effective.ipv6 = enabled(document, "ipv6");
    effective.tun_enabled = enabled(collection(document, "tun", true), "enable");
    const auto mode = string_field(document, "mode");
    if (mode == "rule") {
        effective.routing = routing_mode::rule;
    } else if (mode == "global") {
        effective.routing = routing_mode::global;
    } else if (mode == "direct") {
        effective.routing = routing_mode::direct;
    } else {
        fail(configuration_error::invalid_document);
    }
    if (enabled(document, "allow-lan") &&
        (!document.contains("authentication") || document.at("authentication").empty() ||
         !document.contains("lan-allowed-ips") || document.at("lan-allowed-ips").empty())) {
        fail(configuration_error::invalid_document);
    }
}
// NOLINTEND(llvm-prefer-static-over-anonymous-namespace,altera-struct-pack-align,fuchsia-default-arguments-calls,altera-unroll-loops,readability-redundant-member-init)
} // namespace runtime_config::detail
