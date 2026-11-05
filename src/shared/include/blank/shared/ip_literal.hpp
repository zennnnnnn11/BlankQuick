#ifndef BLANK_SHARED_IP_LITERAL_HPP
#define BLANK_SHARED_IP_LITERAL_HPP

#include <algorithm>
#include <string_view>

namespace blank::net {
// 只判断字面量语法，不解析主机名、不查询网卡作用域、不发 DNS 请求。
// 供配置验证在编译期和纯内存测试中使用；“语法有效”不代表地址在本机可达。
namespace detail {
inline constexpr int decimal_radix{10};
inline constexpr int maximum_octet{255};
inline constexpr int ipv4_octets{4};
inline constexpr int ipv6_groups{8};

// NOLINTBEGIN(fuchsia-default-arguments-calls,altera-unroll-loops)
[[nodiscard]] constexpr auto contains(std::string_view text, std::string_view part) noexcept
    -> bool {
    // ISO C++20 has no string_view::contains; do not add an Abseil dependency.
    // NOLINTNEXTLINE(abseil-string-find-str-contains)
    return text.find(part) != std::string_view::npos;
}

[[nodiscard]] constexpr auto valid_octet(std::string_view part) noexcept -> bool {
    constexpr auto maximum_digits = 3;
    if (part.empty() || part.size() > maximum_digits || (part.size() > 1 && part.front() == '0')) {
        return false;
    }
    auto value = int{};
    for (const auto digit : part) {
        if (digit < '0' || digit > '9') {
            return false;
        }
        value = (value * decimal_radix) + (digit - '0');
    }
    return value <= maximum_octet;
}

[[nodiscard]] constexpr auto valid_hex_group(std::string_view part) noexcept -> bool {
    constexpr auto maximum_digits = 4;
    return part.size() <= maximum_digits &&
           std::ranges::all_of(part, [](char digit) noexcept -> bool {
               return (digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f') ||
                      (digit >= 'A' && digit <= 'F');
           });
}

[[nodiscard]] constexpr auto valid_ipv6_shape(std::string_view text) noexcept -> bool {
    const auto compression = text.find("::");
    return !text.empty() && !contains(text, ":::") &&
           (!text.starts_with(':') || text.starts_with("::")) &&
           (!text.ends_with(':') || text.ends_with("::")) &&
           (compression == std::string_view::npos ||
            text.find("::", compression + 2) == std::string_view::npos);
}
} // namespace detail

// Syntax only: no DNS, sockets, interface enumeration or platform API.
[[nodiscard]] constexpr auto is_ipv4(std::string_view text) noexcept -> bool {
    auto octets = int{};
    while (!text.empty()) {
        const auto separator = text.find('.');
        if (!detail::valid_octet(text.substr(0, separator)) || ++octets > detail::ipv4_octets) {
            return false;
        }
        if (separator == std::string_view::npos) {
            return octets == detail::ipv4_octets;
        }
        text.remove_prefix(separator + 1);
    }
    return false;
}

namespace detail {
[[nodiscard]] constexpr auto ipv6_group_width(std::string_view part, bool last) noexcept -> int {
    if (part.empty()) {
        return 0;
    }
    if (contains(part, ".")) {
        return last && is_ipv4(part) ? 2 : -1;
    }
    return valid_hex_group(part) ? 1 : -1;
}

[[nodiscard]] constexpr auto count_ipv6_groups(std::string_view text) noexcept -> int {
    auto groups = int{};
    while (!text.empty()) {
        const auto separator = text.find(':');
        const auto width =
            ipv6_group_width(text.substr(0, separator), separator == std::string_view::npos);
        if (width < 0) {
            return -1;
        }
        groups += width;
        if (separator == std::string_view::npos) {
            break;
        }
        text.remove_prefix(separator + 1);
    }
    return groups;
}
} // namespace detail

[[nodiscard]] constexpr auto is_ipv6(std::string_view text) noexcept -> bool {
    if (!detail::valid_ipv6_shape(text)) {
        return false;
    }
    const auto groups = detail::count_ipv6_groups(text);
    const bool compressed = detail::contains(text, "::");
    return groups >= 0 &&
           (compressed ? groups < detail::ipv6_groups : groups == detail::ipv6_groups);
}

[[nodiscard]] constexpr auto is_ip_literal(std::string_view text) noexcept -> bool {
    const auto scope = text.find('%');
    if (scope != std::string_view::npos) {
        return scope + 1 < text.size() && is_ipv6(text.substr(0, scope));
    }
    return is_ipv4(text) || is_ipv6(text);
}
// NOLINTEND(fuchsia-default-arguments-calls,altera-unroll-loops)
} // namespace blank::net

#endif
