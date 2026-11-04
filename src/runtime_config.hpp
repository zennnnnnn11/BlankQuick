#ifndef BLANK_QUICK_RUNTIME_CONFIG_HPP // NOLINT(llvm-header-guard)
#define BLANK_QUICK_RUNTIME_CONFIG_HPP

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "minimal_profile.hpp"
#include "override_engine.hpp"

namespace runtime_config {

inline constexpr std::int64_t default_mixed_port{7890};
inline constexpr std::int64_t default_controller_port{9090};
inline constexpr std::chrono::hours default_geo_interval{24};
inline constexpr std::chrono::milliseconds default_lock_timeout{5000};
inline constexpr std::int64_t default_settings_bytes{131072};
inline constexpr std::chrono::milliseconds default_kernel_timeout{10000};
inline constexpr std::int64_t default_kernel_output_bytes{65536};

enum class capture_mode : std::int8_t { proxy, tun };
enum class routing_mode : std::int8_t { rule, global, direct };
enum class log_level : std::int8_t { info, warning, error, debug, silent };
enum class dns_mode : std::int8_t { fake_ip, redir_host, system };
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

// Standard container constructors intentionally use their default allocators.
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
    std::string field{}; // Fixed stage/field only; never a node name, credential or URL.
};

struct effective_configuration {
    std::int64_t mixed_port{};
    bool tun_enabled{};
    bool ipv6{};
    routing_mode routing{routing_mode::rule};
};

struct configuration_result {
    std::string config_json{}; // Candidate only. A successful native check is still required.
    effective_configuration effective{};
    std::vector<configuration_diagnostic> diagnostics{};
    std::vector<overrides::patch_report> patch_reports{};
    configuration_error error{configuration_error::none};
};

enum class settings_error : std::int8_t {
    none,
    invalid_input,
    conflict,
    busy,
    unsafe_path,
    corrupt_settings,
    io_failure,
    allocation_failure,
    unexpected_failure,
};

struct settings_snapshot {
    client_settings settings{};
    std::string revision{}; // Empty means the defaults have never been saved.
};

struct settings_result {
    std::optional<settings_snapshot>
        value{}; // Present only on success; error paths allocate nothing.
    settings_error error{settings_error::none};
};

struct settings_storage_options {
    std::filesystem::path directory{};
    std::chrono::milliseconds lock_timeout{default_lock_timeout};
    std::int64_t max_bytes{default_settings_bytes};
};

enum class kernel_check_error : std::int8_t {
    none,
    invalid_input,
    start_failed,
    rejected,
    timeout,
    cancelled,
    io_failure,
    allocation_failure,
    unexpected_failure,
};

struct kernel_check_options {
    std::filesystem::path executable{};
    std::filesystem::path data_directory{};
    std::chrono::milliseconds timeout{default_kernel_timeout};
    std::int64_t max_output_bytes{default_kernel_output_bytes};
};

struct kernel_check_result {
    kernel_check_error error{kernel_check_error::none};
    std::int64_t exit_code{};
    std::string output{}; // Local debugging only; kernel messages may contain secrets.
    bool output_truncated{};
};
// NOLINTEND(altera-struct-pack-align,readability-redundant-member-init,fuchsia-default-arguments-calls)

// Creates per-instance credentials without touching files, sockets or system settings.
// NOLINTBEGIN(fuchsia-default-arguments-declarations)
[[nodiscard]] auto make_runtime_context(const std::filesystem::path& data_directory,
                                        std::int64_t controller_port = default_controller_port)
    -> runtime_context;

// Pure composition: base + four-part material + supplied patches + final local checks.
// No source material is consumed and no active configuration is mutated on failure.
[[nodiscard]] auto generate(const profile_payload::minimal_profile& material,
                            const client_settings& settings, const runtime_context& context,
                            std::span<const overrides::override_item> patches = {},
                            const generation_limits& limits = {}) noexcept -> configuration_result;
[[nodiscard]] auto generate(const profile_payload::minimal_profile_snapshot& material,
                            const client_settings& settings, const runtime_context& context,
                            std::span<const overrides::override_item> patches = {},
                            const generation_limits& limits = {}) noexcept -> configuration_result;

// Native -t only, never starts the running proxy or modifies system networking.
// The caller supplies the dedicated kernel home containing required GEO/provider assets.
[[nodiscard]] auto check_with_kernel(std::string_view candidate,
                                     const kernel_check_options& options,
                                     const std::stop_token& stop = {}) noexcept
    -> kernel_check_result;
// NOLINTEND(fuchsia-default-arguments-declarations)

// Synchronous worker API. Loading missing settings returns defaults without writing.
// Settings are independent of profiles/overrides; secret and paths are never persisted here.
class settings_manager {
  public:
    explicit settings_manager(settings_storage_options selected);
    [[nodiscard]] auto load() const noexcept -> settings_result;
    [[nodiscard]] auto save(const client_settings& settings,
                            std::string_view expected_revision) const noexcept -> settings_result;

  private:
    settings_storage_options options;
};

} // namespace runtime_config

#endif
