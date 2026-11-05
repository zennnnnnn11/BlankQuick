#ifndef BLANK_QUICK_MINIMAL_PROFILE_HPP // NOLINT(llvm-header-guard)
#define BLANK_QUICK_MINIMAL_PROFILE_HPP

#include <blank/nodes/unified_node.hpp>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace profile_payload {

inline constexpr std::int64_t default_raw_limit{33554432};
inline constexpr std::int64_t default_node_limit{100000};
inline constexpr std::int64_t default_group_limit{10000};
inline constexpr std::int64_t default_rule_limit{100000};
inline constexpr std::int64_t default_provider_limit{10000};
inline constexpr std::int64_t default_reference_limit{1000000};

enum class group_type : std::int8_t { select, url_test, fallback, load_balance };
enum class provider_type : std::int8_t { http, file, inline_payload };
enum class rule_behavior : std::int8_t { domain, ipcidr, classical };
enum class rule_format : std::int8_t { yaml, text, mrs };

// NOLINTBEGIN(altera-struct-pack-align,readability-redundant-member-init)
struct proxy_group {
    std::string name{};
    group_type type{group_type::select};
    std::vector<std::string> proxies{};
    std::string url{};
    std::optional<std::chrono::seconds> interval{};
    std::optional<std::chrono::milliseconds> timeout{};
    std::optional<std::int64_t> max_failed_times{};
    std::string empty_fallback{};
    std::optional<bool> lazy{};
    std::optional<bool> disable_udp{};
    std::string filter{};
    std::string exclude_filter{};
    std::string exclude_type{};
    std::string expected_status{};
    std::optional<bool> include_all_proxies{};
    std::optional<bool> hidden{};
    std::string icon{};
    std::string default_selected{};
    std::optional<std::int64_t> tolerance{};
    std::string strategy{};
    std::string hash_key{};
};

struct rule_provider {
    std::string name{};
    provider_type type{provider_type::http};
    rule_behavior behavior{rule_behavior::domain};
    rule_format format{rule_format::yaml};
    std::string path{};
    std::string url{};
    std::string proxy{};
    std::optional<std::chrono::seconds> interval{};
    std::optional<std::int64_t> size_limit{};
    std::vector<std::string> payload{};
    node_model::string_list_map header{};
    std::string path_in_bundle{};
};

// 节点与路由材料的中间模型，尚未包含客户端监听端口、控制器密钥和 DNS 设置。
// 组装器只解决节点/策略组/规则之间的关系，完整运行配置由 runtime_config 生成。
struct minimal_profile {
    std::vector<node_model::unified_node> proxies{};
    std::vector<proxy_group> proxy_groups{};
    std::vector<std::string> rules{};
    std::vector<rule_provider> rule_providers{};
};

enum class assembly_error : std::int8_t {
    none,
    invalid_input,
    invalid_document,
    invalid_field,
    unsupported_material,
    duplicate_name,
    invalid_reference,
    circular_reference,
    resource_limit,
    allocation_failure,
    unexpected_failure,
};

enum class diagnostic_kind : std::int8_t {
    pruned_reference,
    removed_provider_use,
    empty_group_fallback,
    cleared_default,
    repaired_empty_fallback,
    generated_managed_rules,
    ignored_field,
    informational_node,
};

struct assembly_diagnostic {
    diagnostic_kind kind{};
    std::int64_t index{};
    std::string field{};
};

struct assembly_options {
    bool preserve_original_rules{};
    std::int64_t max_raw_bytes{default_raw_limit};
    std::int64_t max_nodes{default_node_limit};
    std::int64_t max_groups{default_group_limit};
    std::int64_t max_rules{default_rule_limit};
    std::int64_t max_rule_providers{default_provider_limit};
    std::int64_t max_references{default_reference_limit};
};

struct assembly_result {
    minimal_profile profile{};
    std::vector<assembly_diagnostic> diagnostics{};
    assembly_error error{assembly_error::none};
};

using shared_node_pool = std::shared_ptr<const std::vector<node_model::unified_node>>;
// 与拥有节点值的 minimal_profile 等价，但节点共享只读池。反复切换设置或覆写
// 时无需复制协议凭据和大块节点配置；每个节点指针都必须同时维持底层池的所有权。
struct minimal_profile_snapshot {
    std::vector<std::shared_ptr<const node_model::unified_node>> proxies{};
    std::vector<proxy_group> proxy_groups{};
    std::vector<std::string> rules{};
    std::vector<rule_provider> rule_providers{};
};

struct shared_assembly_result {
    minimal_profile_snapshot profile{};
    std::vector<assembly_diagnostic> diagnostics{};
    assembly_error error{assembly_error::none};
};
// NOLINTEND(altera-struct-pack-align,readability-redundant-member-init)

// NOLINTBEGIN(fuchsia-default-arguments-declarations)
[[nodiscard]] auto assemble_minimal_profile(std::vector<node_model::unified_node>&& nodes,
                                            std::string_view raw,
                                            const assembly_options& options = {})
    -> minimal_profile;
[[nodiscard]] auto try_assemble_minimal_profile(std::vector<node_model::unified_node>&& nodes,
                                                std::string_view raw,
                                                const assembly_options& options = {}) noexcept
    -> assembly_result;

[[nodiscard]] auto try_assemble_minimal_profile(const shared_node_pool& nodes, std::string_view raw,
                                                const assembly_options& options = {}) noexcept
    -> shared_assembly_result;
// NOLINTEND(fuchsia-default-arguments-declarations)

} // namespace profile_payload

#endif
