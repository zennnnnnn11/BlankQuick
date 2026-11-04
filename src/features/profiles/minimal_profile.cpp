#include <algorithm>
#include <array>
#include <blank/nodes/clash_material.hpp>
#include <blank/nodes/node_input.hpp>
#include <blank/nodes/unified_node.hpp>
#include <blank/profiles/minimal_profile.hpp>
#include <blank/profiles/node_filter.hpp>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <new>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

// ISO C++ CPU data uses natural alignment, standard defaults and TU-local helpers.
// NOLINTBEGIN(llvm-prefer-static-over-anonymous-namespace,fuchsia-default-arguments-calls,altera-unroll-loops,altera-struct-pack-align,readability-redundant-member-init)
namespace {

using nlohmann::json;
using profile_payload::assembly_error;
using profile_payload::assembly_options;
using profile_payload::assembly_result;
using profile_payload::diagnostic_kind;
using profile_payload::group_type;
using profile_payload::minimal_profile;
using profile_payload::provider_type;
using profile_payload::proxy_group;
using profile_payload::rule_behavior;
using profile_payload::rule_format;
using profile_payload::rule_provider;
using profile_payload::shared_assembly_result;
using profile_payload::shared_node_pool;

using name_set = std::unordered_set<std::string_view>;

constexpr std::string_view automatic_name{"⚡ 自动选择"};
constexpr std::string_view selector_name{"🚀 节点选择"};
constexpr std::string_view test_url{"https://www.gstatic.com/generate_204"};
constexpr std::chrono::seconds test_interval{300};
constexpr std::int64_t maximum_tolerance{65535};
constexpr auto builtins = std::to_array<std::string_view>(
    {"DIRECT", "REJECT", "REJECT-DROP", "COMPATIBLE", "PASS", "PASS-RULE"});

// Copying is required by the standard exception ABI, not polymorphic cloning.
class assembly_failure final : public std::exception { // clazy:exclude=copyable-polymorphic
  public:
    explicit assembly_failure(assembly_error selected) noexcept : error{selected} {}
    [[nodiscard]] auto code() const noexcept -> assembly_error {
        return error;
    }

  private:
    assembly_error error{};
};

[[noreturn]] auto fail(assembly_error error) -> void {
    throw assembly_failure{error};
}

auto notice(assembly_result& result, diagnostic_kind kind, std::int64_t index,
            std::string_view field) -> void {
    result.diagnostics.push_back({.kind = kind, .index = index, .field = std::string{field}});
}

[[nodiscard]] constexpr auto builtin(std::string_view name) noexcept -> bool {
    return std::ranges::find(builtins, name) != builtins.end();
}

[[nodiscard]] constexpr auto trim(std::string_view text) noexcept -> std::string_view {
    constexpr std::string_view whitespace{" \r\n\t"};
    const auto start = text.find_first_not_of(whitespace);
    return start == std::string_view::npos
               ? std::string_view{}
               : text.substr(start, text.find_last_not_of(whitespace) - start + 1);
}

[[nodiscard]] auto text(const json& value) -> std::string {
    if (!value.is_string()) {
        fail(assembly_error::invalid_field);
    }
    return value.get<std::string>();
}

[[nodiscard]] auto get_text(const json& object, std::string_view key) -> std::string {
    const auto found = object.find(key);
    return found == object.end() || found->is_null() ? std::string{} : text(*found);
}

[[nodiscard]] auto list(const json& value) -> std::vector<std::string> {
    if (value.is_null()) {
        return {};
    }
    if (!value.is_array()) {
        fail(assembly_error::invalid_field);
    }
    auto result = std::vector<std::string>{};
    result.reserve(value.size());
    std::ranges::transform(value, std::back_inserter(result), text);
    return result;
}

[[nodiscard]] auto get_list(const json& object, std::string_view key) -> std::vector<std::string> {
    const auto found = object.find(key);
    return found == object.end() ? std::vector<std::string>{} : list(*found);
}

[[nodiscard]] auto get_integer(const json& object, std::string_view key)
    -> std::optional<std::int64_t> {
    const auto found = object.find(key);
    if (found == object.end() || found->is_null()) {
        return std::nullopt;
    }
    const auto token = text(*found);
    auto value = std::int64_t{};
    const auto parsed =
        std::from_chars(std::to_address(token.begin()), std::to_address(token.end()), value);
    if (token.empty() || parsed.ec != std::errc{} || parsed.ptr != std::to_address(token.end()) ||
        value < 0) {
        fail(assembly_error::invalid_field);
    }
    return value;
}

[[nodiscard]] auto get_bool(const json& object, std::string_view key) -> std::optional<bool> {
    const auto found = object.find(key);
    if (found == object.end() || found->is_null()) {
        return std::nullopt;
    }
    const auto token = text(*found);
    if (token == "true" || token == "True" || token == "TRUE") {
        return true;
    }
    if (token == "false" || token == "False" || token == "FALSE") {
        return false;
    }
    fail(assembly_error::invalid_field);
}

[[nodiscard]] auto get_seconds(const json& object, std::string_view key)
    -> std::optional<std::chrono::seconds> {
    const auto value = get_integer(object, key);
    return value ? std::optional{std::chrono::seconds{*value}} : std::nullopt;
}

[[nodiscard]] auto get_milliseconds(const json& object, std::string_view key)
    -> std::optional<std::chrono::milliseconds> {
    const auto value = get_integer(object, key);
    return value ? std::optional{std::chrono::milliseconds{*value}} : std::nullopt;
}

auto require_object(const json& object) -> void {
    if (!object.is_object()) {
        fail(assembly_error::invalid_field);
    }
}

auto unknown_fields(const json& object, std::span<const std::string_view> known,
                    assembly_result& result, std::int64_t index) -> void {
    for (const auto& [key, value] : object.items()) {
        static_cast<void>(value);
        if (std::ranges::find(known, key) == known.end()) {
            // Do not echo arbitrary server keys into logs/diagnostics.
            notice(result, diagnostic_kind::ignored_field, index, "unknown-option");
        }
    }
}

[[nodiscard]] auto parse_group_type(std::string_view type) -> group_type {
    if (type == "select") {
        return group_type::select;
    }
    if (type == "url-test") {
        return group_type::url_test;
    }
    if (type == "fallback") {
        return group_type::fallback;
    }
    if (type == "load-balance") {
        return group_type::load_balance;
    }
    fail(assembly_error::unsupported_material);
}

[[nodiscard]] auto read_group(const json& object, assembly_result& result, std::int64_t index)
    -> proxy_group {
    require_object(object);
    auto group = proxy_group{
        .name = get_text(object, "name"),
        .type = parse_group_type(get_text(object, "type")),
        .proxies = get_list(object, "proxies"),
        .url = get_text(object, "url"),
        .interval = get_seconds(object, "interval"),
        .timeout = get_milliseconds(object, "timeout"),
        .max_failed_times = get_integer(object, "max-failed-times"),
        .empty_fallback = get_text(object, "empty-fallback"),
        .lazy = get_bool(object, "lazy"),
        .disable_udp = get_bool(object, "disable-udp"),
        .filter = get_text(object, "filter"),
        .exclude_filter = get_text(object, "exclude-filter"),
        .exclude_type = get_text(object, "exclude-type"),
        .expected_status = get_text(object, "expected-status"),
        .include_all_proxies = get_bool(object, "include-all-proxies"),
        .hidden = get_bool(object, "hidden"),
        .icon = get_text(object, "icon"),
        .default_selected = get_text(object, "default-selected"),
        .tolerance = get_integer(object, "tolerance"),
        .strategy = get_text(object, "strategy"),
        .hash_key = get_text(object, "hash-key"),
    };
    if (group.tolerance && *group.tolerance > maximum_tolerance) {
        fail(assembly_error::invalid_field);
    }
    if (get_bool(object, "include-all").value_or(false)) {
        group.include_all_proxies = true;
    }
    if (!get_list(object, "use").empty() || get_bool(object, "include-all").value_or(false) ||
        get_bool(object, "include-all-providers").value_or(false)) {
        notice(result, diagnostic_kind::removed_provider_use, index, "use");
    }
    constexpr auto keys = std::to_array<std::string_view>({
        "name",
        "type",
        "proxies",
        "url",
        "interval",
        "timeout",
        "max-failed-times",
        "empty-fallback",
        "lazy",
        "disable-udp",
        "filter",
        "exclude-filter",
        "exclude-type",
        "expected-status",
        "include-all-proxies",
        "hidden",
        "icon",
        "default-selected",
        "tolerance",
        "strategy",
        "hash-key",
        "use",
        "include-all",
        "include-all-providers",
    });
    unknown_fields(object, keys, result, index);
    return group;
}

[[nodiscard]] auto read_provider_type(std::string_view type) -> provider_type {
    if (type == "http") {
        return provider_type::http;
    }
    if (type == "file") {
        return provider_type::file;
    }
    if (type == "inline") {
        return provider_type::inline_payload;
    }
    fail(assembly_error::unsupported_material);
}

[[nodiscard]] auto read_behavior(std::string_view behavior) -> rule_behavior {
    if (behavior == "domain") {
        return rule_behavior::domain;
    }
    if (behavior == "ipcidr") {
        return rule_behavior::ipcidr;
    }
    if (behavior == "classical") {
        return rule_behavior::classical;
    }
    fail(assembly_error::unsupported_material);
}

[[nodiscard]] auto read_format(std::string_view format) -> rule_format {
    if (format.empty() || format == "yaml") {
        return rule_format::yaml;
    }
    if (format == "text") {
        return rule_format::text;
    }
    if (format == "mrs") {
        return rule_format::mrs;
    }
    fail(assembly_error::unsupported_material);
}

[[nodiscard]] auto read_headers(const json& object) -> node_model::string_list_map {
    const auto found = object.find("header");
    if (found == object.end() || found->is_null()) {
        return {};
    }
    require_object(*found);
    auto result = node_model::string_list_map{};
    for (const auto& [key, value] : found->items()) {
        result.emplace_back(key, list(value));
    }
    return result;
}

[[nodiscard]] auto read_provider(std::string name, const json& object, assembly_result& result,
                                 std::int64_t index) -> rule_provider {
    require_object(object);
    auto provider = rule_provider{
        .name = std::move(name),
        .type = read_provider_type(get_text(object, "type")),
        .behavior = read_behavior(get_text(object, "behavior")),
        .format = read_format(get_text(object, "format")),
        .path = get_text(object, "path"),
        .url = get_text(object, "url"),
        .proxy = get_text(object, "proxy"),
        .interval = get_seconds(object, "interval"),
        .size_limit = get_integer(object, "size-limit"),
        .payload = get_list(object, "payload"),
        .header = read_headers(object),
        .path_in_bundle = get_text(object, "path-in-bundle"),
    };
    if ((provider.type == provider_type::http && provider.url.empty()) ||
        (provider.type == provider_type::file && provider.path.empty())) {
        fail(assembly_error::invalid_field);
    }
    constexpr auto keys = std::to_array<std::string_view>({
        "type",
        "behavior",
        "format",
        "path",
        "url",
        "proxy",
        "interval",
        "size-limit",
        "payload",
        "header",
        "path-in-bundle",
    });
    unknown_fields(object, keys, result, index);
    return provider;
}

auto read_groups(const json& sections, assembly_result& result, const assembly_options& options)
    -> void {
    const auto found = sections.find("proxy-groups");
    if (found == sections.end() || found->is_null()) {
        return;
    }
    if (!found->is_array()) {
        fail(assembly_error::invalid_field);
    }
    if (std::ssize(*found) > options.max_groups) {
        fail(assembly_error::resource_limit);
    }
    auto index = std::int64_t{};
    for (const auto& object : *found) {
        result.profile.proxy_groups.push_back(read_group(object, result, ++index));
    }
}

auto read_providers(const json& sections, assembly_result& result, const assembly_options& options)
    -> void {
    const auto found = sections.find("rule-providers");
    if (found == sections.end() || found->is_null()) {
        return;
    }
    require_object(*found);
    if (std::ssize(*found) > options.max_rule_providers) {
        fail(assembly_error::resource_limit);
    }
    auto index = std::int64_t{};
    for (const auto& [name, object] : found->items()) {
        if (name.empty()) {
            fail(assembly_error::invalid_field);
        }
        result.profile.rule_providers.push_back(read_provider(name, object, result, ++index));
    }
}

auto read_rules(const json& sections, minimal_profile& result, const assembly_options& options)
    -> void {
    const auto found = sections.find("rules");
    if (found == sections.end()) {
        return;
    }
    if (!found->is_null() && (!found->is_array() || std::ssize(*found) > options.max_rules)) {
        fail(found->is_array() ? assembly_error::resource_limit : assembly_error::invalid_field);
    }
    result.rules = list(*found);
}

using active_nodes = std::vector<const node_model::unified_node*>;

[[nodiscard]] auto select_nodes(std::span<const node_model::unified_node> nodes, bool managed,
                                assembly_result& result) -> active_nodes {
    auto selected = active_nodes{};
    selected.reserve(nodes.size());
    auto index = std::int64_t{};
    std::ranges::for_each(nodes, [&](const node_model::unified_node& node) -> void {
        ++index;
        if (managed && node_filter::is_informational_node(node)) {
            notice(result, diagnostic_kind::informational_node, index, "proxies");
        } else {
            if (node.core.name.empty() || !node_model::has_matching_protocol(node)) {
                fail(assembly_error::invalid_input);
            }
            selected.push_back(&node);
        }
    });
    return selected;
}

[[nodiscard]] auto node_names(const active_nodes& nodes) -> name_set {
    auto names = name_set{};
    names.reserve(nodes.size());
    for (const auto* node : nodes) {
        if (builtin(node->core.name) || node->core.name == "GLOBAL" ||
            !names.insert(node->core.name).second) {
            fail(assembly_error::duplicate_name);
        }
    }
    return names;
}

auto managed_groups(const active_nodes& nodes, assembly_result& result) -> void {
    auto automatic = proxy_group{
        .name = std::string{automatic_name},
        .type = group_type::url_test,
        .url = std::string{test_url},
        .interval = test_interval,
    };
    auto selector = proxy_group{
        .name = std::string{selector_name},
        .proxies = {std::string{automatic_name}, "DIRECT"},
    };
    automatic.proxies.reserve(nodes.size());
    selector.proxies.reserve(nodes.size() + selector.proxies.size());
    for (const auto* node : nodes) {
        automatic.proxies.push_back(node->core.name);
        selector.proxies.push_back(node->core.name);
    }
    if (automatic.proxies.empty()) {
        automatic.proxies.emplace_back("REJECT");
        notice(result, diagnostic_kind::empty_group_fallback, 1, "proxies");
    }
    result.profile.proxy_groups.push_back(std::move(automatic));
    result.profile.proxy_groups.push_back(std::move(selector));
}

auto managed_rules(minimal_profile& result) -> void {
    constexpr auto rules = std::to_array<std::string_view>({
        "DOMAIN,localhost,DIRECT",
        "DOMAIN-SUFFIX,local,DIRECT",
        "DOMAIN-SUFFIX,lan,DIRECT",
        "IP-CIDR,0.0.0.0/8,DIRECT,no-resolve",
        "IP-CIDR,10.0.0.0/8,DIRECT,no-resolve",
        "IP-CIDR,100.64.0.0/10,DIRECT,no-resolve",
        "IP-CIDR,127.0.0.0/8,DIRECT,no-resolve",
        "IP-CIDR,169.254.0.0/16,DIRECT,no-resolve",
        "IP-CIDR,172.16.0.0/12,DIRECT,no-resolve",
        "IP-CIDR,192.168.0.0/16,DIRECT,no-resolve",
        "IP-CIDR6,::1/128,DIRECT,no-resolve",
        "IP-CIDR6,fc00::/7,DIRECT,no-resolve",
        "IP-CIDR6,fe80::/10,DIRECT,no-resolve",
        "GEOSITE,cn,DIRECT",
        "GEOIP,CN,DIRECT",
    });
    std::ranges::transform(rules, std::back_inserter(result.rules),
                           [](auto rule) -> std::string { return std::string{rule}; });
    result.rules.push_back("MATCH," + std::string{selector_name});
}

[[nodiscard]] auto group_names(const minimal_profile& profile, const name_set& nodes) -> name_set {
    auto names = name_set{};
    names.reserve(profile.proxy_groups.size());
    for (const auto& group : profile.proxy_groups) {
        if (group.name.empty()) {
            fail(assembly_error::invalid_field);
        }
        if (builtin(group.name) || nodes.contains(group.name) || !names.insert(group.name).second) {
            fail(assembly_error::duplicate_name);
        }
    }
    return names;
}

[[nodiscard]] auto known_target(std::string_view name, const name_set& nodes,
                                const name_set& groups) -> bool {
    return builtin(name) || nodes.contains(name) || groups.contains(name);
}

auto prune_groups(assembly_result& result, const name_set& nodes, const name_set& groups) -> void {
    auto index = std::int64_t{};
    for (auto& group : result.profile.proxy_groups) {
        ++index;
        const auto removed = std::erase_if(group.proxies, [&](const std::string& name) -> bool {
            return !known_target(name, nodes, groups);
        });
        if (removed != 0) {
            notice(result, diagnostic_kind::pruned_reference, index, "proxies");
        }
        if (!group.empty_fallback.empty() && !builtin(group.empty_fallback) &&
            !nodes.contains(group.empty_fallback)) {
            group.empty_fallback = "REJECT";
            notice(result, diagnostic_kind::repaired_empty_fallback, index, "empty-fallback");
        }
        if (group.proxies.empty() &&
            (!group.include_all_proxies.value_or(false) || nodes.empty())) {
            group.proxies.emplace_back("REJECT");
            notice(result, diagnostic_kind::empty_group_fallback, index, "proxies");
        }
        if (!group.default_selected.empty() &&
            std::ranges::find(group.proxies, group.default_selected) == group.proxies.end()) {
            group.default_selected.clear();
            notice(result, diagnostic_kind::cleared_default, index, "default-selected");
        }
    }
}

struct reference_graph {
    std::map<std::string_view, std::int64_t, std::less<>> indices{};
    std::vector<std::vector<std::int64_t>> edges{};
    std::vector<std::int64_t> indegrees{};
    std::int64_t references{};
};

struct named_edge {
    std::string_view source{};
    std::string_view target{};
};

auto edge(reference_graph& graph, named_edge connection, std::int64_t limit) -> void {
    if (++graph.references > limit) {
        fail(assembly_error::resource_limit);
    }
    const auto found = graph.indices.find(connection.target);
    if (found == graph.indices.end()) {
        if (!builtin(connection.target)) {
            fail(assembly_error::invalid_reference);
        }
        return;
    }
    graph.edges.at(static_cast<std::size_t>(graph.indices.at(connection.source)))
        .push_back(found->second);
    ++graph.indegrees.at(static_cast<std::size_t>(found->second));
}

auto check_cycles(reference_graph& graph) -> void {
    auto queue = std::vector<std::int64_t>{};
    for (auto index = std::int64_t{}; index < std::ssize(graph.indegrees); ++index) {
        if (graph.indegrees.at(static_cast<std::size_t>(index)) == 0) {
            queue.push_back(index);
        }
    }
    for (auto head = std::int64_t{}; head < std::ssize(queue); ++head) {
        const auto& edges =
            graph.edges.at(static_cast<std::size_t>(queue.at(static_cast<std::size_t>(head))));
        std::ranges::copy_if(edges, std::back_inserter(queue), [&](std::int64_t target) -> bool {
            return --graph.indegrees.at(static_cast<std::size_t>(target)) == 0;
        });
    }
    if (queue.size() != graph.indegrees.size()) {
        fail(assembly_error::circular_reference);
    }
}

auto validate_graph(const minimal_profile& profile, const active_nodes& nodes, std::int64_t limit)
    -> void {
    auto graph = reference_graph{};
    for (const auto* node : nodes) {
        graph.indices.emplace(node->core.name, std::ssize(graph.indices));
    }
    for (const auto& group : profile.proxy_groups) {
        graph.indices.emplace(group.name, std::ssize(graph.indices));
    }
    graph.edges.resize(graph.indices.size());
    graph.indegrees.resize(graph.indices.size());
    for (const auto& group : profile.proxy_groups) {
        for (const auto& target : group.proxies) {
            edge(graph, {.source = group.name, .target = target}, limit);
        }
    }
    for (const auto* node : nodes) {
        if (node->configuration && !node->configuration->link.dialer_proxy.empty()) {
            edge(graph,
                 {.source = node->core.name, .target = node->configuration->link.dialer_proxy},
                 limit);
        }
    }
    check_cycles(graph);
}

[[nodiscard]] auto split_rule(std::string_view rule) -> std::vector<std::string_view> {
    auto parts = std::vector<std::string_view>{};
    while (true) {
        const auto comma = rule.find(',');
        parts.push_back(trim(rule.substr(0, comma)));
        if (comma == std::string_view::npos) {
            return parts;
        }
        rule.remove_prefix(comma + 1);
    }
}

[[nodiscard]] auto uppercase(std::string_view token) -> std::string {
    auto result = std::string{token};
    std::ranges::transform(result, result.begin(), [](char character) noexcept -> char {
        return character >= 'a' && character <= 'z' ? static_cast<char>(character - ('a' - 'A'))
                                                    : character;
    });
    return result;
}

auto validate_rules(const minimal_profile& profile, const name_set& nodes, const name_set& groups)
    -> void {
    auto providers = name_set{};
    for (const auto& provider : profile.rule_providers) {
        providers.insert(provider.name);
        if (!provider.proxy.empty() && provider.proxy != "GLOBAL" &&
            !known_target(provider.proxy, nodes, groups)) {
            fail(assembly_error::invalid_reference);
        }
    }
    for (const auto& rule : profile.rules) {
        const auto parts = split_rule(rule);
        const auto type = uppercase(parts.front());
        if (type == "SUB-RULE") {
            fail(assembly_error::unsupported_material);
        }
        constexpr auto last_target = std::to_array<std::string_view>(
            {"AND", "OR", "NOT", "DOMAIN-REGEX", "PROCESS-NAME-REGEX", "PROCESS-PATH-REGEX"});
        const auto last = std::ranges::find(last_target, type) != last_target.end();
        const auto target_index = type == "MATCH" ? std::size_t{1} : std::size_t{2};
        if (parts.size() <= target_index) {
            fail(assembly_error::invalid_field);
        }
        const auto target = last ? parts.back() : parts.at(target_index);
        if (target != "GLOBAL" && !known_target(target, nodes, groups)) {
            fail(assembly_error::invalid_reference);
        }
        if (type == "RULE-SET" && !providers.contains(parts.at(1))) {
            fail(assembly_error::invalid_reference);
        }
    }
}

auto check_options(std::span<const node_model::unified_node> nodes, std::string_view raw,
                   const assembly_options& options) -> void {
    if (options.max_raw_bytes <= 0 || options.max_nodes <= 0 || options.max_groups <= 0 ||
        options.max_rules <= 0 || options.max_rule_providers <= 0 || options.max_references <= 0) {
        fail(assembly_error::invalid_input);
    }
    if (std::ssize(nodes) > options.max_nodes ||
        (options.preserve_original_rules && std::ssize(raw) > options.max_raw_bytes)) {
        fail(assembly_error::resource_limit);
    }
}

[[nodiscard]] auto original_material(std::string_view raw, const assembly_options& options)
    -> node_input::material::clash_material {
    if (!options.preserve_original_rules) {
        return {};
    }
    auto limits = node_input::parse_limits{};
    limits.max_input_bytes = options.max_raw_bytes;
    limits.max_parameters = std::max(limits.max_parameters, options.max_rule_providers);
    const auto material = node_input::material::extract_clash_material(raw, limits);
    if (material.error == node_input::parse_error::resource_limit) {
        fail(assembly_error::resource_limit);
    }
    if (material.error == node_input::parse_error::allocation_failure) {
        throw std::bad_alloc{};
    }
    if (material.error != node_input::parse_error::none) {
        fail(assembly_error::invalid_document);
    }
    return material;
}

struct assembly_draft {
    assembly_result result{};
    bool managed{};
    active_nodes selected{};
};

[[nodiscard]] auto prepare_draft(std::span<const node_model::unified_node> nodes,
                                 std::string_view raw, const assembly_options& options)
    -> assembly_draft {
    check_options(nodes, raw, options);
    auto result = assembly_result{};
    const auto material = original_material(raw, options);
    const auto managed = !options.preserve_original_rules || !material.found;
    auto selected = select_nodes(nodes, managed, result);
    const auto names = node_names(selected);
    if (managed) {
        managed_groups(selected, result);
        managed_rules(result.profile);
        if (options.preserve_original_rules) {
            notice(result, diagnostic_kind::generated_managed_rules, 0, "rules");
        }
    } else {
        read_groups(material.sections, result, options);
        read_providers(material.sections, result, options);
        read_rules(material.sections, result.profile, options);
    }
    if (std::ssize(result.profile.proxy_groups) > options.max_groups ||
        std::ssize(result.profile.rules) > options.max_rules) {
        fail(assembly_error::resource_limit);
    }
    const auto groups = group_names(result.profile, names);
    if (!managed) {
        prune_groups(result, names, groups);
        validate_rules(result.profile, names, groups);
    }
    const auto chained = std::ranges::any_of(selected, [](const auto* node) noexcept -> bool {
        return node->configuration && !node->configuration->link.dialer_proxy.empty();
    });
    if (!managed || chained) {
        validate_graph(result.profile, selected, options.max_references);
    } else {
        auto references = std::int64_t{};
        for (const auto& group : result.profile.proxy_groups) {
            if (std::ssize(group.proxies) > options.max_references - references) {
                fail(assembly_error::resource_limit);
            }
            references += std::ssize(group.proxies);
        }
    }
    return {.result = std::move(result), .managed = managed, .selected = std::move(selected)};
}

[[nodiscard]] auto build_and_commit(std::vector<node_model::unified_node>&& nodes,
                                    std::string_view raw, const assembly_options& options)
    -> assembly_result {
    auto draft = prepare_draft(nodes, raw, options);
    // All borrowed name indices have been destroyed before any node moves.
    // Commit only after every allocation and validation has succeeded.
    if (draft.managed) {
        auto destination = std::int64_t{};
        for (const auto* selected : draft.selected) {
            const auto source = std::distance(std::to_address(nodes.cbegin()), selected);
            if (source != destination) {
                nodes.at(static_cast<std::size_t>(destination)) =
                    std::move(nodes.at(static_cast<std::size_t>(source)));
            }
            ++destination;
        }
        nodes.erase(nodes.begin() + destination, nodes.end());
    }
    draft.result.profile.proxies = std::move(nodes);
    return std::move(draft.result);
}

[[nodiscard]] auto build_shared(const shared_node_pool& nodes, std::string_view raw,
                                const assembly_options& options) -> shared_assembly_result {
    if (!nodes) {
        fail(assembly_error::invalid_input);
    }
    auto draft = prepare_draft(*nodes, raw, options);
    auto result = shared_assembly_result{};
    result.profile.proxy_groups = std::move(draft.result.profile.proxy_groups);
    result.profile.rules = std::move(draft.result.profile.rules);
    result.profile.rule_providers = std::move(draft.result.profile.rule_providers);
    result.diagnostics = std::move(draft.result.diagnostics);
    result.profile.proxies.reserve(nodes->size());
    for (const auto* node : draft.selected) {
        result.profile.proxies.emplace_back(nodes, node);
    }
    return result;
}

} // namespace
// NOLINTEND(llvm-prefer-static-over-anonymous-namespace,fuchsia-default-arguments-calls,altera-unroll-loops,altera-struct-pack-align,readability-redundant-member-init)

namespace profile_payload {

auto assemble_minimal_profile(std::vector<node_model::unified_node>&& nodes, std::string_view raw,
                              const assembly_options& options) -> minimal_profile {
    return build_and_commit(std::move(nodes), raw, options).profile;
}

auto try_assemble_minimal_profile(std::vector<node_model::unified_node>&& nodes,
                                  std::string_view raw, const assembly_options& options) noexcept
    -> assembly_result {
    try {
        return build_and_commit(std::move(nodes), raw, options);
    } catch (const assembly_failure& failure) {
        return {.error = failure.code()};
    } catch (const std::bad_alloc&) {
        return {.error = assembly_error::allocation_failure};
    } catch (const std::exception&) {
        return {.error = assembly_error::unexpected_failure};
    } catch (...) {
        return {.error = assembly_error::unexpected_failure};
    }
}

auto try_assemble_minimal_profile(const shared_node_pool& nodes, std::string_view raw,
                                  const assembly_options& options) noexcept
    -> shared_assembly_result {
    try {
        return build_shared(nodes, raw, options);
    } catch (const assembly_failure& failure) {
        return {.error = failure.code()};
    } catch (const std::bad_alloc&) {
        return {.error = assembly_error::allocation_failure};
    } catch (const std::exception&) {
        return {.error = assembly_error::unexpected_failure};
    } catch (...) {
        return {.error = assembly_error::unexpected_failure};
    }
}

static_assert(std::is_nothrow_move_constructible_v<assembly_result>);
static_assert(std::is_nothrow_move_assignable_v<node_model::unified_node>);

} // namespace profile_payload
