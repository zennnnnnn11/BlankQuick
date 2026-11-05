#ifndef BLANK_QUICK_RUNTIME_CONFIG_HPP // NOLINT(llvm-header-guard)
#define BLANK_QUICK_RUNTIME_CONFIG_HPP

#include <blank/overrides/override_engine.hpp>
#include <blank/profiles/minimal_profile.hpp>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace runtime_config {

inline constexpr std::int64_t default_mixed_port{7890};
inline constexpr std::int64_t default_controller_port{9090};
inline constexpr std::chrono::hours default_geo_interval{24};

enum class capture_mode : std::int8_t { proxy, tun };
enum class routing_mode : std::int8_t { rule, global, direct };
enum class log_level : std::int8_t { info, warning, error, debug, silent };
enum class dns_mode : std::int8_t { fake_ip, redir_host };
enum class configuration_error : std::int8_t {
    none,
    invalid_settings,
    invalid_context,
    invalid_node,
    invalid_document,
    invalid_reference,
    circular_reference,
    invalid_path,
    protected_field,
    patch_failed,
    resource_limit,
    allocation_failure,
    unexpected_failure,
};

// NOLINTBEGIN(altera-struct-pack-align,readability-redundant-member-init,fuchsia-default-arguments-calls)
struct dns_settings {
    dns_mode mode{dns_mode::fake_ip};
    std::vector<std::string> bootstrap{"223.5.5.5", "119.29.29.29"};
    std::vector<std::string> nameservers{
        "https://dns.alidns.com/dns-query",
        "https://doh.pub/dns-query",
    };
    std::vector<std::string> proxy_nameservers{
        "https://dns.alidns.com/dns-query",
        "https://doh.pub/dns-query",
    };
    std::vector<std::string> direct_nameservers{
        "https://dns.alidns.com/dns-query",
        "https://doh.pub/dns-query",
    };
    std::vector<std::string> fake_ip_filter{
        "+.localhost",           "+.local",        "+.lan",          "+.home.arpa",
        "+.msftconnecttest.com", "+.msftncsi.com", "+.msftnsci.com",
    };
    bool respect_rules{};
    bool append_system_dns{};
};

struct client_settings {
    capture_mode capture{capture_mode::proxy};
    routing_mode routing{routing_mode::rule};
    log_level logging{log_level::info};
    std::int64_t mixed_port{default_mixed_port};
    bool allow_lan{};
    std::vector<std::string> authentication{};
    std::vector<std::string> lan_allowed_ips{};
    bool ipv6{};
    bool tcp_concurrent{true};
    bool unified_delay{};
    bool sniffer{true};
    bool strict_route{true};
    bool geo_auto_update{true};
    std::chrono::hours geo_update_interval{default_geo_interval};
    dns_settings dns{};
};

// 会话分配的执行环境，必须由应用层提供。生成器只使用这些值，不创建目录、
// 不探测端口、不生成随机密钥；相同材料、设置、上下文和覆写得到相同结果。
struct runtime_context {
    std::filesystem::path data_directory{};
    std::string controller{};
    std::string secret{};
};

struct generation_limits {
    std::int64_t max_nodes{profile_payload::default_node_limit};
    std::int64_t max_groups{profile_payload::default_group_limit};
    std::int64_t max_rules{profile_payload::default_rule_limit};
    std::int64_t max_references{profile_payload::default_reference_limit};
    overrides::engine_options patches{};
};

struct configuration_diagnostic {
    configuration_error error{};
    std::int64_t index{};
    std::string field{};
};

// 从整条覆写链的最终文档提取，而不是照抄 client_settings。覆写可以合法改变
// 模式和监听端口，运行调度必须依据真正生效的值安排后续操作。
struct effective_configuration {
    std::int64_t mixed_port{};
    bool tun_enabled{};
    bool ipv6{};
    routing_mode routing{routing_mode::rule};
};

// 失败时 config_json 被清空，effective 也被重置，防止调用者误提交半成品。
// diagnostics 解释失败所在阶段，patch_reports 则保留各条覆写已经产生的报告。
struct configuration_result {
    std::string config_json{};
    effective_configuration effective{};
    std::vector<configuration_diagnostic> diagnostics{};
    std::vector<overrides::patch_report> patch_reports{};
    configuration_error error{configuration_error::none};
};

// NOLINTEND(altera-struct-pack-align,readability-redundant-member-init,fuchsia-default-arguments-calls)

// NOLINTBEGIN(fuchsia-default-arguments-declarations)
// Deterministic in-memory generation supports YAML patches. JavaScript is
// explicitly scheduled by the application through make_draft/finalize_configuration.
[[nodiscard]] auto generate(const profile_payload::minimal_profile& material,
                            const client_settings& settings, const runtime_context& context,
                            std::span<const overrides::override_item> patches = {},
                            const generation_limits& limits = {}) noexcept -> configuration_result;
[[nodiscard]] auto generate(const profile_payload::minimal_profile_snapshot& material,
                            const client_settings& settings, const runtime_context& context,
                            std::span<const overrides::override_item> patches = {},
                            const generation_limits& limits = {}) noexcept -> configuration_result;

// NOLINTEND(fuchsia-default-arguments-declarations)

} // namespace runtime_config

#endif
