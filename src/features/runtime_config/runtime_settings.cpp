#include <QtCore/QRandomGenerator>

#include <algorithm>
#include <array>
#include <blank/runtime_config/runtime_config.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "runtime_config_detail.hpp"

namespace runtime_config::detail {
namespace {
// Documented product defaults below are explicit policy values, not protocol magic.
// NOLINTBEGIN(llvm-prefer-static-over-anonymous-namespace,fuchsia-default-arguments-calls,altera-unroll-loops)
constexpr std::int64_t maximum_port{65535};
constexpr std::int64_t maximum_geo_hours{8760};
constexpr std::size_t maximum_servers{32};
constexpr std::size_t maximum_text_bytes{4096};
constexpr std::size_t maximum_auth_entries{64};
constexpr std::size_t maximum_lan_entries{256};
constexpr std::size_t maximum_credential_bytes{1024};
constexpr std::size_t minimum_secret_bytes{32};
constexpr std::size_t maximum_secret_bytes{256};
constexpr auto capture_names = std::to_array<std::string_view>({"proxy", "tun"});
constexpr auto routing_names = std::to_array<std::string_view>({"rule", "global", "direct"});
constexpr auto log_names =
    std::to_array<std::string_view>({"info", "warning", "error", "debug", "silent"});
constexpr auto dns_names = std::to_array<std::string_view>({"fake-ip", "redir-host", "system"});

template <typename Value, std::size_t Count>
    requires std::is_enum_v<Value>
[[nodiscard]] auto enum_name(Value value, const std::array<std::string_view, Count>& names)
    -> std::string_view {
    const auto index = static_cast<std::int64_t>(value);
    if (index < 0 || std::cmp_greater_equal(index, names.size())) {
        fail(configuration_error::invalid_settings);
    }
    return names.at(static_cast<std::size_t>(index));
}

template <typename Value, std::size_t Count>
    requires std::is_enum_v<Value>
[[nodiscard]] auto read_enum(const json& value, const std::array<std::string_view, Count>& names)
    -> Value {
    if (!value.is_string()) {
        fail(configuration_error::invalid_settings);
    }
    const auto found = std::ranges::find(names, value.get_ref<const std::string&>());
    if (found == names.end()) {
        fail(configuration_error::invalid_settings);
    }
    return static_cast<Value>(std::distance(names.begin(), found));
}

[[nodiscard]] constexpr auto clean_text(std::string_view value) noexcept -> bool {
    return !value.empty() && value.find_first_of("\r\n\0", 0, 3) == std::string_view::npos;
}

[[nodiscard]] constexpr auto valid_controller(std::string_view controller) noexcept -> bool {
    constexpr std::string_view prefix{"127.0.0.1:"};
    if (!controller.starts_with(prefix)) {
        return false;
    }
    controller.remove_prefix(prefix.size());
    constexpr std::int64_t decimal_radix{10};
    auto port = std::int64_t{};
    for (const auto character : controller) {
        if (character < '0' || character > '9') {
            return false;
        }
        port = (port * decimal_radix) +
               (static_cast<std::int64_t>(character) - static_cast<std::int64_t>('0'));
        if (port > maximum_port) {
            return false;
        }
    }
    return port > 0;
}

auto check_servers(const std::vector<std::string>& servers, bool required) -> void {
    if ((required && servers.empty()) || servers.size() > maximum_servers) {
        fail(configuration_error::invalid_settings);
    }
    for (const auto& server : servers) {
        if (!clean_text(server) || server.size() > maximum_text_bytes) {
            fail(configuration_error::invalid_settings);
        }
    }
}

auto require_fields(const json& value, std::span<const std::string_view> fields) -> void {
    if (!value.is_object() || value.size() != fields.size()) {
        fail(configuration_error::invalid_settings);
    }
    for (const auto field : fields) {
        if (!value.contains(field)) {
            fail(configuration_error::invalid_settings);
        }
    }
}

[[nodiscard]] auto read_integer(const json& value) -> std::int64_t {
    if (!value.is_number_integer()) {
        fail(configuration_error::invalid_settings);
    }
    if (value.is_number_unsigned() &&
        value.get<std::uint64_t>() >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        fail(configuration_error::invalid_settings);
    }
    return value.get<std::int64_t>();
}

[[nodiscard]] auto dns_document(const dns_settings& dns) -> json {
    return {
        {"mode", enum_name(dns.mode, dns_names)},
        {"bootstrap", dns.bootstrap},
        {"nameservers", dns.nameservers},
        {"proxy_nameservers", dns.proxy_nameservers},
        {"direct_nameservers", dns.direct_nameservers},
        {"fake_ip_filter", dns.fake_ip_filter},
        {"respect_rules", dns.respect_rules},
    };
}
} // namespace

auto validate_settings(const client_settings& settings) -> void {
    static_cast<void>(enum_name(settings.capture, capture_names));
    static_cast<void>(enum_name(settings.routing, routing_names));
    static_cast<void>(enum_name(settings.logging, log_names));
    static_cast<void>(enum_name(settings.dns.mode, dns_names));
    if (settings.mixed_port < 1 || settings.mixed_port > maximum_port ||
        settings.geo_update_interval.count() < 1 ||
        settings.geo_update_interval.count() > maximum_geo_hours ||
        (settings.capture == capture_mode::tun && settings.dns.mode == dns_mode::system)) {
        fail(configuration_error::invalid_settings);
    }
    if (settings.authentication.size() > maximum_auth_entries ||
        settings.lan_allowed_ips.size() > maximum_lan_entries ||
        (settings.allow_lan &&
         (settings.authentication.empty() || settings.lan_allowed_ips.empty()))) {
        fail(configuration_error::invalid_settings);
    }
    for (const auto& item : settings.authentication) {
        const auto colon = item.find(':');
        if (!clean_text(item) || item.size() > maximum_credential_bytes || colon == 0 ||
            colon == std::string::npos || colon + 1 == item.size()) {
            fail(configuration_error::invalid_settings);
        }
    }
    for (const auto& address : settings.lan_allowed_ips) {
        if (!clean_text(address) || address.size() > maximum_lan_entries) {
            fail(configuration_error::invalid_settings);
        }
    }
    if (settings.dns.mode != dns_mode::system) {
        check_servers(settings.dns.bootstrap, true);
        check_servers(settings.dns.nameservers, true);
        check_servers(settings.dns.proxy_nameservers, settings.dns.respect_rules);
        check_servers(settings.dns.direct_nameservers, false);
    }
    if (settings.dns.fake_ip_filter.size() > maximum_text_bytes) {
        fail(configuration_error::invalid_settings);
    }
    for (const auto& item : settings.dns.fake_ip_filter) {
        if (!clean_text(item) || item.size() > maximum_text_bytes) {
            fail(configuration_error::invalid_settings);
        }
    }
}

auto settings_document(const client_settings& settings) -> json {
    validate_settings(settings);
    return {
        {"capture", enum_name(settings.capture, capture_names)},
        {"routing", enum_name(settings.routing, routing_names)},
        {"logging", enum_name(settings.logging, log_names)},
        {"mixed_port", settings.mixed_port},
        {"allow_lan", settings.allow_lan},
        {"authentication", settings.authentication},
        {"lan_allowed_ips", settings.lan_allowed_ips},
        {"ipv6", settings.ipv6},
        {"tcp_concurrent", settings.tcp_concurrent},
        {"unified_delay", settings.unified_delay},
        {"sniffer", settings.sniffer},
        {"strict_route", settings.strict_route},
        {"geo_auto_update", settings.geo_auto_update},
        {"geo_update_interval_hours", settings.geo_update_interval.count()},
        {"dns", dns_document(settings.dns)},
    };
}

auto settings_from_document(const json& document) -> client_settings {
    constexpr auto keys = std::to_array<std::string_view>({
        "capture",
        "routing",
        "logging",
        "mixed_port",
        "allow_lan",
        "authentication",
        "lan_allowed_ips",
        "ipv6",
        "tcp_concurrent",
        "unified_delay",
        "sniffer",
        "strict_route",
        "geo_auto_update",
        "geo_update_interval_hours",
        "dns",
    });
    require_fields(document, keys);
    auto result = client_settings{};
    result.capture = read_enum<capture_mode>(document.at("capture"), capture_names);
    result.routing = read_enum<routing_mode>(document.at("routing"), routing_names);
    result.logging = read_enum<log_level>(document.at("logging"), log_names);
    result.mixed_port = read_integer(document.at("mixed_port"));
    result.allow_lan = document.at("allow_lan").get<bool>();
    result.authentication = document.at("authentication").get<std::vector<std::string>>();
    result.lan_allowed_ips = document.at("lan_allowed_ips").get<std::vector<std::string>>();
    result.ipv6 = document.at("ipv6").get<bool>();
    result.tcp_concurrent = document.at("tcp_concurrent").get<bool>();
    result.unified_delay = document.at("unified_delay").get<bool>();
    result.sniffer = document.at("sniffer").get<bool>();
    result.strict_route = document.at("strict_route").get<bool>();
    result.geo_auto_update = document.at("geo_auto_update").get<bool>();
    result.geo_update_interval =
        std::chrono::hours{read_integer(document.at("geo_update_interval_hours"))};
    constexpr auto dns_keys = std::to_array<std::string_view>({
        "mode",
        "bootstrap",
        "nameservers",
        "proxy_nameservers",
        "direct_nameservers",
        "fake_ip_filter",
        "respect_rules",
    });
    const auto& dns = document.at("dns");
    require_fields(dns, dns_keys);
    result.dns.mode = read_enum<dns_mode>(dns.at("mode"), dns_names);
    result.dns.bootstrap = dns.at("bootstrap").get<std::vector<std::string>>();
    result.dns.nameservers = dns.at("nameservers").get<std::vector<std::string>>();
    result.dns.proxy_nameservers = dns.at("proxy_nameservers").get<std::vector<std::string>>();
    result.dns.direct_nameservers = dns.at("direct_nameservers").get<std::vector<std::string>>();
    result.dns.fake_ip_filter = dns.at("fake_ip_filter").get<std::vector<std::string>>();
    result.dns.respect_rules = dns.at("respect_rules").get<bool>();
    validate_settings(result);
    return result;
}

// JSON subscripts intentionally create named object properties.
// NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
// These literals are auditable DNS/cache and well-known sniffed-port policy.
// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
auto base_document(const client_settings& settings, const runtime_context& context) -> json {
    validate_settings(settings);
    if (!context.data_directory.is_absolute() ||
        context.data_directory == context.data_directory.root_path() ||
        !valid_controller(context.controller) || context.secret.size() < minimum_secret_bytes ||
        context.secret.size() > maximum_secret_bytes || !clean_text(context.secret)) {
        fail(configuration_error::invalid_context);
    }
    auto result = json{
        {"mixed-port", settings.mixed_port},
        {"allow-lan", settings.allow_lan},
        {"mode", enum_name(settings.routing, routing_names)},
        {"log-level", enum_name(settings.logging, log_names)},
        {"ipv6", settings.ipv6},
        {"find-process-mode", "strict"},
        {"tcp-concurrent", settings.tcp_concurrent},
        {"unified-delay", settings.unified_delay},
        {"disable-keep-alive", false},
        {"etag-support", true},
        {"profile", {{"store-selected", true}, {"store-fake-ip", true}}},
        {"external-controller", context.controller},
        {"secret", context.secret},
        {
            "external-controller-cors",
            {
                {"allow-origins", json::array({"http://" + context.controller})},
                {"allow-private-network", false},
            },
        },
        {"geodata-mode", true},
        {"geodata-loader", "memconservative"},
        {"geosite-matcher", "succinct"},
        {"geo-auto-update", settings.geo_auto_update},
        {"geo-update-interval", settings.geo_update_interval.count()},
    };
    if (settings.allow_lan) {
        result["authentication"] = settings.authentication;
        result["lan-allowed-ips"] = settings.lan_allowed_ips;
    }
    result["tun"] = {
        {"enable", settings.capture == capture_mode::tun},
        {"stack", "mips"},
        {"auto-route", true},
        {"auto-detect-interface", true},
        {"strict-route", settings.strict_route},
        {"dns-mode", "hijack"},
        {"dns-hijack", json::array({"any:53", "tcp://any:53"})},
    };
    if (settings.dns.mode == dns_mode::system) {
        result["dns"] = {{"enable", false}};
    } else {
        result["dns"] = {
            {"enable", true},
            {"listen", "127.0.0.1:1053"},
            {"ipv6", settings.ipv6},
            {"use-hosts", true},
            {"use-system-hosts", true},
            {"prefer-h3", false},
            {"cache-algorithm", "lru"},
            {"cache-max-size", 4096},
            {"respect-rules", settings.dns.respect_rules},
            {"enhanced-mode", enum_name(settings.dns.mode, dns_names)},
            {"default-nameserver", settings.dns.bootstrap},
            {"nameserver", settings.dns.nameservers},
            {"proxy-server-nameserver", settings.dns.proxy_nameservers},
            {"direct-nameserver", settings.dns.direct_nameservers},
            {"direct-nameserver-follow-policy", false},
        };
        if (settings.dns.mode == dns_mode::fake_ip) {
            result["dns"]["fake-ip-range"] = "198.18.0.1/16";
            result["dns"]["fake-ip-filter-mode"] = "blacklist";
            result["dns"]["fake-ip-filter"] = settings.dns.fake_ip_filter;
            if (settings.ipv6) {
                result["dns"]["fake-ip-range6"] = "fdfe:dcba:9876::1/64";
            }
        }
    }
    result["sniffer"] = {
        {"enable", settings.sniffer},
        {"force-dns-mapping", false},
        {"parse-pure-ip", true},
        {"override-destination", false},
        {
            "sniff",
            {
                {"HTTP", {{"ports", json::array({80, 8080, 8880})}}},
                {"TLS", {{"ports", json::array({443, 8443})}}},
                {"QUIC", {{"ports", json::array({443, 8443})}}},
            },
        },
        {
            "skip-dst-address",
            json::array({
                "127.0.0.0/8",
                "10.0.0.0/8",
                "172.16.0.0/12",
                "192.168.0.0/16",
                "169.254.0.0/16",
            }),
        },
    };
    return result;
}
// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
// NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
} // namespace runtime_config::detail

namespace runtime_config {
auto make_runtime_context(const std::filesystem::path& data_directory, std::int64_t controller_port)
    -> runtime_context {
    if (!data_directory.is_absolute() || data_directory == data_directory.root_path() ||
        controller_port < 1 || controller_port > detail::maximum_port) {
        throw std::invalid_argument{"Invalid runtime context"};
    }
    constexpr std::string_view hex{"0123456789abcdef"};
    constexpr std::size_t secret_size{64};
    constexpr std::int64_t word_count{8};
    constexpr std::int64_t highest_nibble_shift{28};
    constexpr std::uint32_t nibble_mask{15U};
    auto secret = std::string{};
    secret.reserve(secret_size);
    for (std::int64_t index{}; index < word_count; ++index) {
        const auto value = QRandomGenerator::system()->generate();
        for (std::int64_t shift{highest_nibble_shift}; shift >= 0; shift -= 4) {
            secret.push_back(hex.at(static_cast<std::size_t>(
                (value >> static_cast<unsigned int>(shift)) & nibble_mask)));
        }
    }
    return {
        .data_directory = data_directory.lexically_normal(),
        .controller = "127.0.0.1:" + std::to_string(controller_port),
        .secret = std::move(secret),
    };
}
// NOLINTEND(llvm-prefer-static-over-anonymous-namespace,fuchsia-default-arguments-calls,altera-unroll-loops)
} // namespace runtime_config
