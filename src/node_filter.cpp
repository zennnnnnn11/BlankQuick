#include "node_filter.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <ranges>
#include <string_view>
#include <system_error>
#include <variant>

#include "unified_node.hpp"

// ISO C++ internals have translation-unit scope; fixed loops target CPUs and use standard defaults.
// NOLINTBEGIN(llvm-prefer-static-over-anonymous-namespace,altera-unroll-loops,fuchsia-default-arguments-calls)
namespace {

constexpr int octet_radix{256};
constexpr int ipv4_loopback_prefix{127};
constexpr int ipv6_word_count{8};
constexpr int ipv6_marker_index{5};
constexpr int ipv6_ipv4_high_index{6};
constexpr int ipv6_low_index{7};
constexpr int mapped_ipv4_marker{65535};

[[nodiscard]] constexpr auto ascii_lower(char character) noexcept -> char {
    return character >= 'A' && character <= 'Z' ? static_cast<char>(character + ('a' - 'A'))
                                                : character;
}

[[nodiscard]] constexpr auto ascii_word(char character) noexcept -> bool {
    const auto lower = ascii_lower(character);
    return (lower >= 'a' && lower <= 'z') || (lower >= '0' && lower <= '9');
}

[[nodiscard]] constexpr auto same_text(std::string_view text, std::string_view token) noexcept
    -> bool {
    return std::ranges::equal(text, token, {}, ascii_lower, ascii_lower);
}

[[nodiscard]] constexpr auto contains_text(std::string_view text, std::string_view token) noexcept
    -> bool {
    return !std::ranges::search(text, token, {}, ascii_lower, ascii_lower).empty();
}

// Haystack/needle order mirrors standard search; no ownership or interchangeable roles.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
[[nodiscard]] constexpr auto contains_word(std::string_view text, std::string_view word) noexcept
    -> bool {
    auto remaining = text;
    while (!remaining.empty()) {
        const auto match = std::ranges::search(remaining, word, {}, ascii_lower, ascii_lower);
        if (match.empty()) {
            return false;
        }
        if ((match.begin() == remaining.begin() || !ascii_word(*std::prev(match.begin()))) &&
            (match.end() == remaining.end() || !ascii_word(*match.end()))) {
            return true;
        }
        remaining = std::string_view{match.end(), remaining.end()};
    }
    return false;
}

[[nodiscard]] constexpr auto trim(std::string_view text) noexcept -> std::string_view {
    constexpr std::string_view spaces{" \t\r\n\f\v"};
    const auto start = text.find_first_not_of(spaces);
    if (start == std::string_view::npos) {
        return {};
    }
    const auto end = text.find_last_not_of(spaces);
    return text.substr(start, end - start + 1);
}

[[nodiscard]] constexpr auto announcement_name(std::string_view name) noexcept -> bool {
    // Avoid broad matches such as 流量/群/traffic/reset: normal routes use those labels too.
    constexpr auto phrases = std::to_array<std::string_view>({
        "剩余流量",
        "剩餘流量",
        "已用流量",
        "流量：",
        "流量:",
        "流量剩余",
        "流量剩餘",
        "流量重置",
        "流量用尽",
        "流量耗尽",
        "流量耗盡",
        "到期",
        "已过期",
        "已過期",
        "重置剩余",
        "重置剩餘",
        "下次重置",
        "重置：",
        "重置:",
        "官网",
        "官網",
        "续费",
        "續費",
        "套餐",
        "公告",
        "通知",
        "官方群",
        "交流群",
        "群号",
        "群號",
        "群：",
        "群:",
        "用户群",
        "用戶群",
        "加入群",
        "加群",
        "加入QQ群",
        "加入TG群",
        "订阅更新",
        "訂閱更新",
        "更新订阅",
        "更新訂閱",
        "下载客户端",
        "下載客戶端",
        "建议：",
        "建议:",
        "建議：",
        "建議:",
        "traffic remaining",
        "remaining traffic",
        "traffic used",
        "used traffic",
        "traffic left",
        "traffic reset",
        "traffic:",
        "traffic：",
        "reset traffic",
        "data remaining",
        "remaining data",
        "data usage",
        "data used",
        "bandwidth remaining",
        "official website",
        "renew subscription",
        "subscription expired",
        "download client",
        "reset in",
        "next reset",
        "reset:",
        "reset：",
        "expires in",
        "expires on",
        "expiry date",
        "expiration date",
        "expire date",
        "expire:",
        "expire：",
        "expires:",
        "expiry:",
    });
    constexpr auto words = std::to_array<std::string_view>({"expired", "announcement", "notice"});
    return std::ranges::any_of(
               phrases,
               [name](auto phrase) noexcept -> bool { return contains_text(name, phrase); }) ||
           std::ranges::any_of(
               words, [name](auto word) noexcept -> bool { return contains_word(name, word); });
}

[[nodiscard]] auto decimal_octet(std::string_view text) noexcept -> std::optional<int> {
    if (text.empty() || text.size() > 3 || text.front() == '-') {
        return std::nullopt;
    }
    auto value = int{};
    const auto result =
        std::from_chars(std::to_address(text.begin()), std::to_address(text.end()), value);
    if (result.ec != std::errc{} || result.ptr != std::to_address(text.end()) || value < 0 ||
        value >= octet_radix) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] auto ipv4(std::string_view text) noexcept -> std::optional<std::array<int, 4>> {
    auto octets = std::array<int, 4>{};
    for (auto& octet : octets) {
        const auto dot = text.find('.');
        const auto value = decimal_octet(text.substr(0, dot));
        if (!value || ((&octet != &octets.back()) != (dot != std::string_view::npos))) {
            return std::nullopt;
        }
        octet = *value;
        text.remove_prefix(dot == std::string_view::npos ? text.size() : dot + 1);
    }
    return octets;
}

[[nodiscard]] constexpr auto local_ipv4(const std::array<int, 4>& octets) noexcept -> bool {
    return octets.front() == ipv4_loopback_prefix ||
           std::ranges::all_of(octets, [](int octet) noexcept -> bool { return octet == 0; });
}

[[nodiscard]] auto hex_word(std::string_view token) noexcept -> std::optional<int> {
    if (token.empty() || token.size() > 4 || token.front() == '-') {
        return std::nullopt;
    }
    auto value = int{};
    constexpr int hex_base{16};
    const auto result = std::from_chars(std::to_address(token.begin()),
                                        std::to_address(token.end()), value, hex_base);
    if (result.ec != std::errc{} || result.ptr != std::to_address(token.end()) || value < 0) {
        return std::nullopt;
    }
    return value;
}

// Expanded and compressed IPv6, including IPv4-mapped loopback/unspecified addresses.
[[nodiscard]] auto ipv6_words(std::string_view text) noexcept
    -> std::optional<std::array<int, ipv6_word_count>> {
    auto words = std::array<int, ipv6_word_count>{};
    auto count = std::int64_t{};
    auto compression = std::int64_t{-1};
    if (text.starts_with("::")) {
        compression = 0;
        text.remove_prefix(2);
    }
    while (!text.empty()) {
        if (count == ipv6_word_count) {
            return std::nullopt;
        }
        const auto colon = text.find(':');
        const auto token = text.substr(0, colon);
        if (contains_text(token, ".")) {
            const auto octets = ipv4(token);
            if (!octets || count > ipv6_ipv4_high_index || colon != std::string_view::npos) {
                return std::nullopt;
            }
            words.at(static_cast<std::size_t>(count++)) =
                (octets->at(0) * octet_radix) + octets->at(1);
            words.at(static_cast<std::size_t>(count++)) =
                (octets->at(2) * octet_radix) + octets->at(3);
            break;
        }
        const auto value = hex_word(token);
        if (!value) {
            return std::nullopt;
        }
        words.at(static_cast<std::size_t>(count++)) = *value;
        if (colon == std::string_view::npos) {
            break;
        }
        text.remove_prefix(colon + 1);
        if (text.starts_with(':')) {
            if (compression >= 0) {
                return std::nullopt;
            }
            compression = count;
            text.remove_prefix(1);
        } else if (text.empty()) {
            return std::nullopt;
        }
    }
    if (compression < 0) {
        return count == ipv6_word_count ? std::optional{words} : std::nullopt;
    }
    if (count == ipv6_word_count) {
        return std::nullopt;
    }
    const auto padding = ipv6_word_count - count;
    for (auto index = count - 1; index >= compression; --index) {
        words.at(static_cast<std::size_t>(index + padding)) =
            words.at(static_cast<std::size_t>(index));
        words.at(static_cast<std::size_t>(index)) = 0;
    }
    return words;
}

[[nodiscard]] auto local_ipv6(std::string_view host) noexcept -> bool {
    if (host.starts_with('[') && host.ends_with(']')) {
        host.remove_prefix(1);
        host.remove_suffix(1);
    }
    const auto zone = host.find('%');
    if (zone != std::string_view::npos) {
        host = host.substr(0, zone);
    }
    const auto words = ipv6_words(host);
    if (!words || !std::ranges::all_of(*words | std::views::take(ipv6_marker_index),
                                       [](int word) noexcept -> bool { return word == 0; })) {
        return false;
    }
    return (words->at(ipv6_marker_index) == 0 && words->at(ipv6_ipv4_high_index) == 0 &&
            words->at(ipv6_low_index) <= 1) ||
           (words->at(ipv6_marker_index) == mapped_ipv4_marker &&
            ((words->at(ipv6_ipv4_high_index) / octet_radix == ipv4_loopback_prefix) ||
             (words->at(ipv6_ipv4_high_index) == 0 && words->at(ipv6_low_index) == 0)));
}

[[nodiscard]] auto classify_host(std::string_view host) noexcept
    -> node_filter::informational_reason {
    using node_filter::informational_reason;
    host = trim(host);
    if (host.empty()) {
        return informational_reason::missing_endpoint;
    }
    if (host.ends_with('.')) {
        host.remove_suffix(1);
    }
    constexpr std::string_view local_suffix{".localhost"};
    if (same_text(host, "localhost") ||
        (host.size() > local_suffix.size() &&
         same_text(host.substr(host.size() - local_suffix.size()), local_suffix))) {
        return informational_reason::local_endpoint;
    }
    if (const auto octets = ipv4(host); octets && local_ipv4(*octets)) {
        return informational_reason::local_endpoint;
    }
    if (local_ipv6(host)) {
        return informational_reason::local_endpoint;
    }
    constexpr auto placeholders = std::to_array<std::string_view>(
        {"null", "none", "undefined", "-", "待更新", "请更新订阅", "請更新訂閱"});
    if (std::ranges::any_of(
            placeholders, [host](auto token) noexcept -> bool { return same_text(host, token); }) ||
        (host.find_first_of(".:/") == std::string_view::npos && announcement_name(host))) {
        return informational_reason::placeholder_endpoint;
    }
    return informational_reason::none;
}

[[nodiscard]] constexpr auto endpoint_required(node_model::protocol_type protocol) noexcept
    -> bool {
    using node_model::protocol_type;
    return protocol == protocol_type::vless || protocol == protocol_type::vmess ||
           protocol == protocol_type::shadowsocks || protocol == protocol_type::trojan ||
           protocol == protocol_type::hysteria2 || protocol == protocol_type::tuic ||
           protocol == protocol_type::wireguard || protocol == protocol_type::anytls;
}

[[nodiscard]] auto classify_endpoints(const node_model::unified_node& node) noexcept
    -> node_filter::informational_reason {
    using node_filter::informational_reason;
    if (node.core.protocol == node_model::protocol_type::wireguard && node.configuration) {
        const auto* parameters =
            std::get_if<node_model::wireguard_param>(&node.configuration->detail);
        if (parameters != nullptr && !parameters->peers.empty()) {
            if (std::ranges::any_of(parameters->peers, [](const auto& peer) noexcept -> bool {
                    return classify_host(peer.server) == informational_reason::none;
                })) {
                return informational_reason::none;
            }
            return classify_host(parameters->peers.front().server);
        }
    }
    if (node.core.remote) {
        return classify_host(node.core.remote->host);
    }
    return endpoint_required(node.core.protocol) ? informational_reason::missing_endpoint
                                                 : informational_reason::none;
}

} // namespace
// NOLINTEND(llvm-prefer-static-over-anonymous-namespace,altera-unroll-loops,fuchsia-default-arguments-calls)

namespace node_filter {

auto classify_informational_node(const node_model::unified_node& node) noexcept
    -> informational_reason {
    using node_model::protocol_type;
    if (node.core.protocol == protocol_type::unspecified ||
        node.core.protocol == protocol_type::direct || node.core.protocol == protocol_type::dns ||
        node.core.protocol == protocol_type::reject ||
        node.core.protocol == protocol_type::rematch) {
        return informational_reason::none;
    }
    if (announcement_name(node.core.name)) {
        return informational_reason::announcement_name;
    }
    return classify_endpoints(node);
}

} // namespace node_filter
