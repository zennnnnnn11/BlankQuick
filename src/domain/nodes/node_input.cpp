#include <algorithm>
#include <array>
#include <blank/nodes/clash_material.hpp>
#include <blank/nodes/node_input.hpp>
#include <blank/nodes/unified_node.hpp>
#include <blank/shared/bounded_json.hpp>
#include <charconv>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <nlohmann/json.hpp>
#include <optional>
#include <ranges>
#include <rapidyaml/ryml.hpp> // IWYU pragma: keep; private YAML adapter uses this API.
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace node_input {
namespace {

// NOLINTBEGIN(llvm-prefer-static-over-anonymous-namespace, fuchsia-default-arguments-calls,

using nlohmann::json;
using node_model::node_configuration;
using node_model::protocol_type;

enum class uri_protocol : std::int8_t {
    unspecified,
    vless,
    vmess,
    shadowsocks,
    trojan,
    hysteria2,
    tuic,
    wireguard,
    anytls,
};

[[nodiscard]] constexpr auto model_protocol(uri_protocol protocol) noexcept -> protocol_type {
    switch (protocol) {
    case uri_protocol::unspecified:
        return protocol_type::unspecified;
    case uri_protocol::vless:
        return protocol_type::vless;
    case uri_protocol::vmess:
        return protocol_type::vmess;
    case uri_protocol::shadowsocks:
        return protocol_type::shadowsocks;
    case uri_protocol::trojan:
        return protocol_type::trojan;
    case uri_protocol::hysteria2:
        return protocol_type::hysteria2;
    case uri_protocol::tuic:
        return protocol_type::tuic;
    case uri_protocol::wireguard:
        return protocol_type::wireguard;
    case uri_protocol::anytls:
        return protocol_type::anytls;
    }
    return protocol_type::unspecified;
}

constexpr std::int64_t maximum_port{65535};
constexpr std::int64_t tls_port{443};
constexpr std::int64_t wireguard_port{51820};
constexpr std::int64_t milliseconds_per_second{1000};
constexpr std::int64_t wireguard_key_bytes{32};
constexpr std::int64_t wireguard_reserved_bytes{3};
constexpr std::int64_t maximum_byte{255};
constexpr std::int64_t maximum_warning_key{80};

struct input_failure : std::exception {
    explicit input_failure(parse_error code) noexcept : error{code} {}
    parse_error error{parse_error::invalid_parameter};
};

[[noreturn]] auto fail(parse_error error) -> void {
    throw input_failure{error};
}

[[nodiscard]] constexpr auto ascii_space(char value) noexcept -> bool {
    return value == ' ' || value == '\t' || value == '\n' || value == '\r';
}

[[nodiscard]] constexpr auto trim(std::string_view value) noexcept -> std::string_view {
    while (!value.empty() && ascii_space(value.front())) {
        value.remove_prefix(1);
    }
    while (!value.empty() && ascii_space(value.back())) {
        value.remove_suffix(1);
    }
    return value;
}

[[nodiscard]] constexpr auto lower_ascii(char value) noexcept -> char {
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
}

[[nodiscard]] auto lower(std::string_view value) -> std::string {
    auto result = std::string{value};
    std::ranges::transform(result, result.begin(), lower_ascii);
    return result;
}

[[nodiscard]] auto normalize_key(std::string_view value) -> std::string {
    auto result = std::string{};
    for (const auto character : value) {
        if (character != '-' && character != '_') {
            result.push_back(lower_ascii(character));
        }
    }
    return result;
}

[[nodiscard]] constexpr auto hex_digit(char value) noexcept -> int {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    constexpr int hexadecimal_offset{10};
    const auto folded = lower_ascii(value);
    return folded >= 'a' && folded <= 'f' ? folded - 'a' + hexadecimal_offset : -1;
}

struct utf8_lead {
    std::uint32_t first{};
    std::uint32_t last{};
    std::uint32_t mask{};
    std::uint32_t minimum{};
    int count{};
};

[[nodiscard]] auto utf8_prefix(std::uint32_t first) -> utf8_lead {
    constexpr auto leads = std::to_array<utf8_lead>({
        {.first = 0xc2, .last = 0xdf, .mask = 0x1f, .minimum = 0x80, .count = 1},
        {.first = 0xe0, .last = 0xef, .mask = 0x0f, .minimum = 0x800, .count = 2},
        {.first = 0xf0, .last = 0xf4, .mask = 0x07, .minimum = 0x10000, .count = 3},
    });
    const auto found = std::ranges::find_if(leads, [&](const auto& lead) noexcept -> bool {
        return first >= lead.first && first <= lead.last;
    });
    if (found == leads.end()) {
        fail(parse_error::invalid_encoding);
    }
    return *found;
}

auto validate_text(std::string_view value, bool multiline = false) -> void {
    constexpr std::uint32_t continuation_mask{0x3f};
    constexpr std::uint32_t continuation_tag{0x80};
    constexpr std::uint32_t continuation_prefix_mask{0xc0};
    constexpr std::uint32_t maximum_codepoint{0x10ffff};
    constexpr std::uint32_t surrogate_first{0xd800};
    constexpr std::uint32_t surrogate_last{0xdfff};
    constexpr unsigned char delete_character{0x7f};
    constexpr unsigned int continuation_bits{6};
    auto remaining = value;
    while (!remaining.empty()) {
        const auto first = static_cast<unsigned char>(remaining.front());
        remaining.remove_prefix(1);
        if (first < continuation_tag) {
            if ((first < ' ' && !(multiline && ascii_space(static_cast<char>(first)))) ||
                first == delete_character) {
                fail(parse_error::invalid_encoding);
            }
            continue;
        }
        const auto lead = utf8_prefix(first);
        auto codepoint = first & lead.mask;
        for (auto index = 0; index < lead.count; ++index) {
            if (remaining.empty()) {
                fail(parse_error::invalid_encoding);
            }
            const auto next = static_cast<unsigned char>(remaining.front());
            remaining.remove_prefix(1);
            if ((next & continuation_prefix_mask) != continuation_tag) {
                fail(parse_error::invalid_encoding);
            }
            codepoint = (codepoint << continuation_bits) | (next & continuation_mask);
        }
        if (codepoint < lead.minimum || codepoint > maximum_codepoint ||
            (codepoint >= surrogate_first && codepoint <= surrogate_last)) {
            fail(parse_error::invalid_encoding);
        }
    }
}

[[nodiscard]] auto percent_decode(std::string_view value) -> std::string {
    constexpr int hexadecimal_radix{16};
    auto result = std::string{};
    result.reserve(value.size());
    while (!value.empty()) {
        if (value.front() != '%') {
            result.push_back(value.front());
            value.remove_prefix(1);
            continue;
        }
        constexpr std::size_t escape_size{3};
        if (value.size() < escape_size) {
            fail(parse_error::invalid_encoding);
        }
        const auto high = hex_digit(value.at(1));
        const auto low = hex_digit(value.at(2));
        if (high < 0 || low < 0) {
            fail(parse_error::invalid_encoding);
        }
        result.push_back(static_cast<char>((high * hexadecimal_radix) + low));
        value.remove_prefix(escape_size);
    }
    validate_text(result);
    return result;
}

[[nodiscard]] constexpr auto base64_digit(char value) noexcept -> int {
    constexpr std::string_view alphabet{
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"};
    if (value == '-') {
        value = '+';
    } else if (value == '_') {
        value = '/';
    }
    const auto position = alphabet.find(value);
    return position == std::string_view::npos ? -1 : static_cast<int>(position);
}

[[nodiscard]] auto decode_base64(std::string_view value, bool allow_whitespace = false)
    -> std::string {
    constexpr unsigned int sextet_bits{6};
    constexpr unsigned int octet_bits{8};
    constexpr int quartet_size{4};
    constexpr std::uint32_t octet_mask{0xff};
    auto result = std::string{};
    result.reserve(value.size());
    auto accumulator = std::uint32_t{};
    auto bits = std::uint32_t{};
    auto digits = std::int64_t{};
    auto padding = int{};
    for (const auto character : value) {
        if (allow_whitespace && ascii_space(character)) {
            continue;
        }
        if (character == '=') {
            ++padding;
            if (padding > 2) {
                fail(parse_error::invalid_encoding);
            }
            continue;
        }
        const auto digit = base64_digit(character);
        if (digit < 0 || padding != 0) {
            fail(parse_error::invalid_encoding);
        }
        ++digits;
        accumulator = (accumulator << sextet_bits) | static_cast<std::uint32_t>(digit);
        bits += sextet_bits;
        if (bits >= octet_bits) {
            bits -= octet_bits;
            result.push_back(static_cast<char>((accumulator >> bits) & octet_mask));
        }
    }
    const auto tail = digits % quartet_size;
    if (digits == 0 || tail == 1 || (padding != 0 && (digits + padding) % quartet_size != 0) ||
        (bits != 0 && (accumulator & ((1U << bits) - 1U)) != 0)) {
        fail(parse_error::invalid_encoding);
    }
    return result;
}

[[nodiscard]] auto split(std::string_view value, char delimiter) -> std::vector<std::string> {
    auto result = std::vector<std::string>{};
    while (true) {
        const auto position = value.find(delimiter);
        result.emplace_back(trim(value.substr(0, position)));
        if (position == std::string_view::npos) {
            return result;
        }
        value.remove_prefix(position + 1);
    }
}

[[nodiscard]] auto integer(std::string_view value) -> std::int64_t {
    value = trim(value);
    if (value.empty()) {
        fail(parse_error::invalid_parameter);
    }
    auto result = std::int64_t{};
    const auto parsed =
        std::from_chars(std::to_address(value.begin()), std::to_address(value.end()), result);
    if (parsed.ec != std::errc{} || parsed.ptr != std::to_address(value.end())) {
        fail(parse_error::invalid_parameter);
    }
    return result;
}

[[nodiscard]] auto scalar_string(const json& value) -> std::string {
    if (value.is_string()) {
        auto result = value.get<std::string>();
        validate_text(result);
        return result;
    }
    if (value.is_number_integer() || value.is_boolean()) {
        return value.dump();
    }
    fail(parse_error::invalid_parameter);
}

auto read_value(const json& value, std::string& result) -> void {
    result = scalar_string(value);
}

auto read_pem(const json& value, std::string& result) -> void {
    if (!value.is_string()) {
        fail(parse_error::invalid_parameter);
    }
    result = value.get<std::string>();
    validate_text(result, true);
}

auto read_pem(const json& value, std::optional<std::string>& result) -> void {
    auto text = std::string{};
    read_pem(value, text);
    result = std::move(text);
}

auto read_value(const json& value, std::int64_t& result) -> void {
    result = integer(scalar_string(value));
    if (result < 0) {
        fail(parse_error::invalid_parameter);
    }
}

auto read_value(const json& value, bool& result) -> void {
    const auto text = lower(scalar_string(value));
    if (text == "true" || text == "1" || text == "yes" || text == "on") {
        result = true;
    } else if (text == "false" || text == "0" || text == "no" || text == "off") {
        result = false;
    } else {
        fail(parse_error::invalid_parameter);
    }
}

auto read_value(const json& value, std::vector<std::string>& result) -> void {
    if (value.is_array()) {
        std::ranges::transform(value, std::back_inserter(result), scalar_string);
    } else {
        result = split(scalar_string(value), ',');
    }
}

auto read_value(const json& value, node_model::string_map& result) -> void {
    if (!value.is_object()) {
        fail(parse_error::invalid_parameter);
    }
    for (const auto& [key, entry] : value.items()) {
        validate_text(key);
        result.emplace_back(key, scalar_string(entry));
    }
}

auto read_value(const json& value, node_model::string_list_map& result) -> void {
    if (!value.is_object()) {
        fail(parse_error::invalid_parameter);
    }
    for (const auto& [key, entry] : value.items()) {
        validate_text(key);
        auto strings = std::vector<std::string>{};
        read_value(entry, strings);
        result.emplace_back(key, std::move(strings));
    }
}

[[nodiscard]] auto duration_count(const json& value, std::int64_t units_per_second)
    -> std::int64_t {
    auto text = scalar_string(value);
    auto multiplier = std::int64_t{1};
    if (text.ends_with("ms")) {
        text.resize(text.size() - 2);
        if (units_per_second == 1) {
            const auto count = integer(text);
            if (count < 0 || count % milliseconds_per_second != 0) {
                fail(parse_error::invalid_parameter);
            }
            return count / milliseconds_per_second;
        }
    } else if (text.ends_with('s')) {
        text.pop_back();
        multiplier = units_per_second;
    } else if (text.ends_with('m')) {
        constexpr std::int64_t seconds_per_minute{60};
        text.pop_back();
        multiplier = units_per_second * seconds_per_minute;
    }
    const auto count = integer(text);
    if (count < 0 || count > std::numeric_limits<std::int64_t>::max() / multiplier) {
        fail(parse_error::invalid_parameter);
    }
    return count * multiplier;
}

auto read_value(const json& value, std::chrono::seconds& result) -> void {
    result = std::chrono::seconds{duration_count(value, 1)};
}

auto read_value(const json& value, std::chrono::milliseconds& result) -> void {
    result = std::chrono::milliseconds{duration_count(value, milliseconds_per_second)};
}

auto read_value(const json& value, node_model::hopping_interval& result) -> void {
    const auto text = scalar_string(value);
    if (text.find('-') != std::string::npos) {
        const auto ends = split(text, '-');
        if (ends.size() != 2 || integer(ends.front()) <= 0 ||
            integer(ends.front()) > integer(ends.back())) {
            fail(parse_error::invalid_parameter);
        }
        result = text;
    } else {
        result = std::chrono::seconds{duration_count(value, 1)};
    }
}

auto read_value(const json& value, node_model::ip_preference& result) -> void {
    const auto text = normalize_key(scalar_string(value));
    if (text == "dual") {
        result = node_model::ip_preference::dual;
    } else if (text == "ipv4" || text == "4") {
        result = node_model::ip_preference::ipv4;
    } else if (text == "ipv6" || text == "6") {
        result = node_model::ip_preference::ipv6;
    } else if (text == "ipv4prefer") {
        result = node_model::ip_preference::ipv4_prefer;
    } else if (text == "ipv6prefer") {
        result = node_model::ip_preference::ipv6_prefer;
    } else {
        fail(parse_error::invalid_parameter);
    }
}

auto read_value(const json& value, std::vector<std::byte>& result) -> void {
    auto bytes = json{};
    if (value.is_array()) {
        bytes = value;
    } else {
        const auto text = scalar_string(value);
        if (text.starts_with('[')) {
            bytes = json::parse(text);
        } else if (text.find(',') != std::string::npos) {
            bytes = split(text, ',');
        } else {
            const auto decoded = decode_base64(text);
            std::ranges::transform(
                decoded, std::back_inserter(result), [](char character) noexcept -> std::byte {
                    return static_cast<std::byte>(static_cast<unsigned char>(character));
                });
            return;
        }
    }
    if (!bytes.is_array()) {
        fail(parse_error::invalid_parameter);
    }
    for (const auto& entry : bytes) {
        const auto number = integer(scalar_string(entry));
        if (number < 0 || number > maximum_byte) {
            fail(parse_error::invalid_parameter);
        }
        result.push_back(static_cast<std::byte>(number));
    }
}

auto read_value(const json& value, std::vector<int>& result) -> void {
    if (!value.is_array()) {
        fail(parse_error::invalid_parameter);
    }
    for (const auto& entry : value) {
        const auto number = integer(scalar_string(entry));
        if (number < 0 || number > std::numeric_limits<int>::max()) {
            fail(parse_error::invalid_parameter);
        }
        result.push_back(static_cast<int>(number));
    }
}

template <typename value_type>
    requires requires(const json& value, value_type& result) { read_value(value, result); }
auto read_member(const json& object, std::string_view key, value_type& target) -> void {
    if (!object.is_object()) {
        fail(parse_error::invalid_parameter);
    }
    for (const auto& [name, value] : object.items()) {
        if (normalize_key(name) == normalize_key(key)) {
            read_value(value, target);
        }
    }
}

auto read_value(const json& value, std::vector<node_model::tls_mirror_traffic_step>& result)
    -> void {
    if (!value.is_array()) {
        fail(parse_error::invalid_parameter);
    }
    for (const auto& entry : value) {
        auto step = node_model::tls_mirror_traffic_step{};
        read_member(entry, "name", step.name);
        read_member(entry, "host", step.host);
        read_member(entry, "path", step.path);
        read_member(entry, "method", step.method);
        read_member(entry, "connection-ready", step.connection_ready);
        read_member(entry, "connection-recall-exit", step.connection_recall_exit);
        read_member(entry, "h2-do-not-wait-for-download-finish",
                    step.h2_do_not_wait_for_download_finish);
        for (const auto& [key, child] : entry.items()) {
            const auto normalized = normalize_key(key);
            if (normalized == "waittime") {
                read_member(child, "base-nanoseconds", step.wait_time.base_nanoseconds);
                read_member(child, "uniform-random-multiplier-nanoseconds",
                            step.wait_time.uniform_random_multiplier_nanoseconds);
            } else if (normalized == "headers") {
                if (!child.is_array()) {
                    fail(parse_error::invalid_parameter);
                }
                for (const auto& item : child) {
                    auto header = node_model::tls_mirror_traffic_header{};
                    read_member(item, "name", header.name);
                    read_member(item, "value", header.value);
                    read_member(item, "values", header.values);
                    step.headers.push_back(std::move(header));
                }
            } else if (normalized == "nextstep") {
                if (!child.is_array()) {
                    fail(parse_error::invalid_parameter);
                }
                for (const auto& item : child) {
                    auto candidate = node_model::tls_mirror_traffic_transfer_candidate{};
                    read_member(item, "weight", candidate.weight);
                    read_member(item, "goto-location", candidate.goto_location);
                    step.next_step.push_back(candidate);
                }
            }
        }
        result.push_back(std::move(step));
    }
}

template <typename value_type>
    requires std::default_initializable<value_type>
auto read_value(const json& value, std::optional<value_type>& result) -> void {
    auto decoded = value_type{};
    read_value(value, decoded);
    result = std::move(decoded);
}

template <typename value_type>
    requires std::default_initializable<value_type>
[[nodiscard]] auto present(std::optional<value_type>& value) noexcept(
    std::is_nothrow_default_constructible_v<value_type>) -> value_type& {
    if (!value.has_value()) {
        value.emplace();
    }
    return *value;
}

struct option_value {
    json value{};
    bool consumed{};
};

struct options { // NOLINT(bugprone-exception-escape)
    // 各格式先把别名和嵌套字段整理到统一键空间。consumed 记录字段是否真正
    // 参与了模型构造，剩余字段进入 ignored_parameters，而不是悄悄丢失。
    std::map<std::string, option_value, std::less<>> values{};
    parse_limits limits{};
};

[[nodiscard]] auto bounded_json(std::string_view text, const parse_limits& limits) -> json {
    auto parsed = json_detail::read<json>(
        text, {.max_event_depth = limits.max_json_depth, .ignore_comments = true});
    if (parsed.error == json_detail::read_error::resource_limit) {
        fail(parse_error::resource_limit);
    }
    if (parsed.error != json_detail::read_error::none) {

        return json::parse(text, nullptr, true, true);
    }
    return std::move(parsed.value);
}

// NOLINTNEXTLINE(misc-no-recursion)
auto set_option(options& target, std::string_view key, const json& value) -> void {
    const auto normalized = normalize_key(key);
    target.values.insert_or_assign(normalized, option_value{.value = value, .consumed = false});
    if (std::ssize(target.values) > target.limits.max_parameters) {
        fail(parse_error::resource_limit);
    }
    if (value.is_object()) {
        for (const auto& [child_key, child] : value.items()) {
            set_option(target, normalized + '.' + child_key, child);
        }
    }
}

[[nodiscard]] auto find_option(options& source, std::string_view key) -> const json* {
    // 读取同时标记已消费；只想查看存在性而不认可语义时，文档适配器使用
    // peek_option。二者不能随意互换，否则会错误隐藏“尚未支持的字段”诊断。
    const auto found = source.values.find(normalize_key(key));
    if (found == source.values.end()) {
        return nullptr;
    }
    found->second.consumed = true;
    return &found->second.value;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
[[nodiscard]] auto option_text(options& source, std::string_view key,
                               std::string_view fallback = {}) -> std::string {
    const auto* value = find_option(source, key);
    return value == nullptr ? std::string{fallback} : scalar_string(*value);
}

template <typename value_type>
    requires requires(const json& value, value_type& result) { read_value(value, result); }
auto read_option(options& source, std::string_view key, value_type& target) -> void {
    if (const auto* value = find_option(source, key)) {
        read_value(*value, target);
    }
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
auto alias(options& source, std::string_view from, std::string_view destination) -> void {
    // 显式的目标字段优先，别名只补缺省值；复制后再插入，避免容器修改使
    // from 对应的观察指针失效。别名被识别后也算已消费，不重复报告成未知字段。
    const auto* value = find_option(source, from);
    if (value != nullptr && !source.values.contains(normalize_key(destination))) {
        const auto copy = *value;
        set_option(source, destination, copy);
    }
}

[[nodiscard]] auto structured_parameter(std::string_view key) -> bool {
    const auto normalized = normalize_key(key);
    return normalized == "extra" || normalized == "ech" || normalized == "smux" ||
           normalized == "mux" || normalized == "headers" || normalized == "peers" ||
           normalized == "reserved" || normalized == "alpn" || normalized == "dns" ||
           normalized == "address" || normalized == "localaddress" || normalized == "allowedips" ||
           normalized.ends_with("opts") || normalized.ends_with("settings") ||
           normalized.ends_with(".headers");
}

auto parse_query(options& target, std::string_view query) -> void {
    auto parameter_count = std::int64_t{};
    for (const auto& item : split(query, '&')) {
        if (item.empty()) {
            continue;
        }
        ++parameter_count;
        if (parameter_count > target.limits.max_parameters) {
            fail(parse_error::resource_limit);
        }
        const auto equal = item.find('=');
        const auto key = percent_decode(std::string_view{item}.substr(0, equal));
        const auto value = equal == std::string::npos
                               ? std::string{"true"}
                               : percent_decode(std::string_view{item}.substr(equal + 1));
        const auto content = trim(value);
        if (normalize_key(key) == "stun" && target.values.contains("stun")) {
            auto entries = target.values.at("stun").value;
            if (!entries.is_array()) {
                entries = json::array({entries});
            }
            entries.push_back(value);
            set_option(target, key, entries);
        } else if (structured_parameter(key) &&
                   (content.starts_with('{') || content.starts_with('['))) {
            set_option(target, key, bounded_json(content, target.limits));
        } else {
            set_option(target, key, value);
        }
    }
}

[[nodiscard]] auto parse_protocol(std::string_view scheme) -> uri_protocol {
    const auto text = lower(scheme);
    if (text == "vless") {
        return uri_protocol::vless;
    }
    if (text == "vmess") {
        return uri_protocol::vmess;
    }
    if (text == "ss") {
        return uri_protocol::shadowsocks;
    }
    if (text == "trojan") {
        return uri_protocol::trojan;
    }
    if (text == "hysteria2" || text == "hy2" || text == "hysteria2+realm" || text == "hy2+realm") {
        return uri_protocol::hysteria2;
    }
    if (text == "tuic") {
        return uri_protocol::tuic;
    }
    if (text == "wireguard" || text == "wg") {
        return uri_protocol::wireguard;
    }
    if (text == "anytls") {
        return uri_protocol::anytls;
    }
    fail(parse_error::unsupported_protocol);
}

[[nodiscard]] auto valid_ipv4(std::string_view value) -> bool {
    const auto parts = split(value, '.');
    constexpr std::ptrdiff_t octets{4};
    if (std::ssize(parts) != octets) {
        return false;
    }
    return std::ranges::all_of(parts, [](const auto& part) noexcept -> bool {
        auto number = int{};
        const auto parsed = std::from_chars(part.data(), std::to_address(part.end()), number);
        return !part.empty() && !(part.size() > 1 && part.front() == '0') &&
               parsed.ec == std::errc{} && parsed.ptr == std::to_address(part.end()) &&
               number >= 0 && number <= maximum_byte;
    });
}

[[nodiscard]] auto valid_ipv6(std::string_view value) -> bool {
    constexpr std::int64_t ipv6_groups{8};
    constexpr std::size_t maximum_group_digits{4};
    const auto compression = value.find("::");
    if (value.empty() || value.find(":::") != std::string_view::npos ||
        (compression != std::string_view::npos &&
         value.find("::", compression + 2) != std::string_view::npos)) {
        return false;
    }
    auto groups = std::int64_t{};
    for (const auto& part : split(value, ':')) {
        if (part.empty()) {
            continue;
        }
        if (part.find('.') != std::string::npos) {
            if (!value.ends_with(part) || !valid_ipv4(part)) {
                return false;
            }
            groups += 2;
        } else {
            if (part.size() > maximum_group_digits ||
                !std::ranges::all_of(
                    part, [](char digit) noexcept -> bool { return hex_digit(digit) >= 0; })) {
                return false;
            }
            ++groups;
        }
    }
    if (compression == std::string_view::npos) {
        return !value.starts_with(':') && !value.ends_with(':') && groups == ipv6_groups;
    }
    return !value.starts_with(":::") && !value.ends_with(":::") && groups < ipv6_groups;
}

[[nodiscard]] auto endpoint(std::string_view authority, std::int64_t default_port)
    -> node_model::remote_endpoint {
    const auto colon = authority.rfind(':');
    auto host = authority.substr(0, colon);
    auto port = std::string_view{};
    auto explicit_port = false;
    if (authority.starts_with('[')) {
        const auto close = authority.find(']');
        if (close == std::string_view::npos) {
            fail(parse_error::invalid_endpoint);
        }
        host = authority.substr(1, close - 1);
        if (!valid_ipv6(host)) {
            fail(parse_error::invalid_endpoint);
        }
        const auto tail = authority.substr(close + 1);
        if (!tail.empty()) {
            if (!tail.starts_with(':')) {
                fail(parse_error::invalid_endpoint);
            }
            explicit_port = true;
            port = tail.substr(1);
        }
    } else {
        explicit_port = colon != std::string_view::npos;
        if (explicit_port) {
            port = authority.substr(colon + 1);
        }
        if (host.find(':') != std::string_view::npos) {
            fail(parse_error::invalid_endpoint);
        }
    }
    const auto decoded_host = percent_decode(host);
    if (decoded_host.empty() || decoded_host.find_first_of(" /?#@[]\\") != std::string::npos) {
        fail(parse_error::invalid_endpoint);
    }
    auto port_number = default_port;
    if (explicit_port) {
        if (port.empty() || !std::ranges::all_of(port, [](char digit) noexcept -> bool {
                return digit >= '0' && digit <= '9';
            })) {
            fail(parse_error::invalid_endpoint);
        }
        port_number = integer(port);
    }
    if (port_number <= 0 || port_number > maximum_port) {
        fail(parse_error::invalid_endpoint);
    }
    return {.host = decoded_host, .port = port_number};
}

auto read_value(const json& value, std::vector<node_model::wireguard_peer_param>& result) -> void {
    if (!value.is_array()) {
        fail(parse_error::invalid_parameter);
    }
    for (const auto& item : value) {
        if (!item.is_object()) {
            fail(parse_error::invalid_parameter);
        }
        auto source = options{};
        for (const auto& [key, entry] : item.items()) {
            set_option(source, key, entry);
        }
        auto peer = node_model::wireguard_peer_param{};
        read_option(source, "server", peer.server);
        read_option(source, "port", peer.port);
        read_option(source, "public-key", peer.public_key);
        read_option(source, "pre-shared-key", peer.pre_shared_key);
        read_option(source, "reserved", peer.reserved);
        read_option(source, "allowed-ips", peer.allowed_ips);
        result.push_back(std::move(peer));
    }
}

#include "node_input_bindings.inc"

auto normalize_common(options& source, uri_protocol protocol) -> void {
    constexpr auto aliases = std::to_array<std::pair<std::string_view, std::string_view>>({
        {"sni", "servername"},
        {"peer", "servername"},
        {"amp;sni", "servername"},
        {"amp;servername", "servername"},
        {"insecure", "skip-cert-verify"},
        {"allowInsecure", "skip-cert-verify"},
        {"fp", "client-fingerprint"},
        {"certificate-sha256", "fingerprint"},
        {"cert-sha256", "fingerprint"},
        {"ca-sha256", "fingerprint"},
        {"pinSHA256", "fingerprint"},
        {"pinnedCertSha256", "fingerprint"},
        {"pcs", "fingerprint"},
        {"hpkp", "fingerprint"},
        {"vcn", "name-cert-verify"},
        {"pbk", "reality-opts.public-key"},
        {"public-key", "reality-opts.public-key"},
        {"sid", "reality-opts.short-id"},
        {"short-id", "reality-opts.short-id"},
        {"amp;pbk", "reality-opts.public-key"},
        {"amp;sid", "reality-opts.short-id"},
        {"support-x25519mlkem768", "reality-opts.support-x25519mlkem768"},
        {"packetEncoding", "packet-encoding"},
        {"uot", "udp-over-tcp"},
        {"uot-version", "udp-over-tcp-version"},
        {"mux.enabled", "smux.enabled"},
        {"mux.protocol", "smux.protocol"},
        {"mux.max-connections", "smux.max-connections"},
        {"mux.min-streams", "smux.min-streams"},
        {"mux.max-streams", "smux.max-streams"},
        {"mux.padding", "smux.padding"},
        {"mux", "smux.enabled"},
        {"brutal", "smux.brutal-opts.enabled"},
        {"brutal-up", "smux.brutal-opts.up"},
        {"brutal-down", "smux.brutal-opts.down"},
    });
    for (const auto& item : aliases) {
        const auto from = item.first;
        const auto destination = item.second;

        if (from != "public-key") {
            alias(source, from, destination);
        }
    }
    if (const auto* tls = find_option(source, "tls")) {
        if (tls->is_string()) {
            const auto text = lower(scalar_string(*tls));
            if (text == "tls" || text == "reality") {
                set_option(source, "tls", true);
                if (text == "reality") {
                    set_option(source, "security", "reality");
                }
            } else if (text.empty() || text == "none") {
                set_option(source, "tls", false);
            }
        }
    }
    if (const auto* security = find_option(source, "security")) {
        const auto text = lower(scalar_string(*security));
        if (text == "tls" || text == "reality") {
            set_option(source, "tls", true);
        } else if (text == "none" || text.empty()) {
            set_option(source, "tls", false);
        } else if (protocol == uri_protocol::vmess &&
                   (text == "auto" || text == "aes-128-gcm" || text == "chacha20-poly1305")) {
            alias(source, "security", "cipher");
        } else {
            fail(parse_error::unsupported_parameter);
        }
    }
    if (const auto* encoding = find_option(source, "packet-encoding")) {
        const auto text = lower(scalar_string(*encoding));
        if (text == "packet" || text == "packetaddr") {
            set_option(source, "packet-addr", true);
            set_option(source, "xudp", false);
        } else if (text == "xudp") {
            set_option(source, "xudp", true);
        } else if (text != "none" && !text.empty()) {
            fail(parse_error::unsupported_parameter);
        }
    }
    if (const auto* ech = find_option(source, "ech")) {
        if (ech->is_object()) {
            set_option(source, "ech-opts", *ech);
        } else {
            set_option(source, "ech-opts.enable", true);
            set_option(source, "ech-opts.config", *ech);
        }
    }
}

[[nodiscard]] auto normalize_xhttp_key(std::string_view key) -> std::string {
    const auto normalized = normalize_key(key);
    constexpr auto aliases = std::to_array<std::pair<std::string_view, std::string_view>>({
        {"xmux", "reuse-settings"},
        {"downloadsettings", "download-settings"},
        {"sessionidplacement", "session-placement"},
        {"sessionidkey", "session-key"},
        {"sessionidlength", "session-length"},
        {"sessionidtable", "session-table"},
        {"address", "server"},
        {"tlsettings", "tls-settings"},
    });
    for (const auto& item : aliases) {
        const auto from = item.first;
        const auto destination = item.second;
        if (normalized == from) {
            return std::string{destination};
        }
    }
    return std::string{key};
}

// NOLINTNEXTLINE(misc-no-recursion, readability-function-cognitive-complexity)
auto import_xhttp_extra(options& source, std::string_view prefix, const json& extra) -> void {
    if (!extra.is_object()) {
        fail(parse_error::invalid_parameter);
    }
    for (const auto& [key, value] : extra.items()) {
        const auto normalized = normalize_key(key);
        if (normalized == "tlssettings" || normalized == "realitysettings") {
            if (!value.is_object()) {
                fail(parse_error::invalid_parameter);
            }
            set_option(source, std::string{prefix} + ".tls", true);
            for (const auto& [child_key, child] : value.items()) {
                const auto folded = normalize_key(child_key);
                auto destination = std::string{prefix} + '.' + child_key;
                if (folded == "fingerprint") {
                    destination = std::string{prefix} + ".client-fingerprint";
                } else if (folded == "allowinsecure") {
                    destination = std::string{prefix} + ".skip-cert-verify";
                } else if (folded == "publickey" || folded == "shortid") {
                    destination = std::string{prefix} + ".reality-opts." + child_key;
                }
                set_option(source, destination, child);
            }
        } else if (normalized == "security") {
            const auto security = lower(scalar_string(value));
            if (security != "tls" && security != "reality" && security != "none") {
                fail(parse_error::unsupported_parameter);
            }
            set_option(source, std::string{prefix} + ".tls", security != "none");
        } else if (normalized == "network") {
            if (lower(scalar_string(value)) != "xhttp") {
                fail(parse_error::unsupported_parameter);
            }
        } else if (normalized == "xhttpsettings" || normalized == "extra") {
            import_xhttp_extra(source, prefix, value);
        } else if (value.is_object() && normalized != "headers") {
            import_xhttp_extra(source, std::string{prefix} + '.' + normalize_xhttp_key(key), value);
        } else {
            set_option(source, std::string{prefix} + '.' + normalize_xhttp_key(key), value);
        }
    }
}

auto websocket_early_data(options& source) -> void {
    const auto path = option_text(source, "ws-opts.path");
    const auto question = path.find('?');
    if (question == std::string::npos) {
        return;
    }
    auto kept = path.substr(0, question);
    auto first = true;
    for (const auto& item : split(std::string_view{path}.substr(question + 1), '&')) {
        const auto equal = item.find('=');
        if (item.substr(0, equal) == "ed" && equal != std::string::npos) {
            set_option(source, "ws-opts.max-early-data", item.substr(equal + 1));
        } else {
            kept += first ? '?' : '&';
            kept += item;
            first = false;
        }
    }
    set_option(source, "ws-opts.path", kept);
}

auto configure_websocket(options& source, node_model::transport_payload& transport,
                         std::string_view network) -> void {
    transport = node_model::websocket_transport{};
    alias(source, "path", "ws-opts.path");
    alias(source, "ws-path", "ws-opts.path");
    alias(source, "httpupgrade-opts.path", "ws-opts.path");
    alias(source, "host", "ws-opts.headers.Host");
    alias(source, "httpupgrade-opts.host", "ws-opts.headers.Host");
    alias(source, "ws-headers.Host", "ws-opts.headers.Host");
    alias(source, "ws-headers", "ws-opts.headers");
    alias(source, "headers", "ws-opts.headers");
    alias(source, "httpupgrade-opts.headers", "ws-opts.headers");
    alias(source, "ed", "ws-opts.max-early-data");
    alias(source, "early-data", "ws-opts.max-early-data");
    alias(source, "max-early-data", "ws-opts.max-early-data");
    alias(source, "eh", "ws-opts.early-data-header-name");
    alias(source, "early-data-header-name", "ws-opts.early-data-header-name");
    websocket_early_data(source);
    if (network == "httpupgrade") {
        set_option(source, "ws-opts.v2ray-http-upgrade", true);
    }
    if (const auto* host = find_option(source, "ws-opts.headers.Host")) {
        auto headers = json::object();
        if (const auto* previous = find_option(source, "ws-opts.headers")) {
            headers = *previous;
        }
        headers.update(json{{"Host", *host}});
        set_option(source, "ws-opts.headers", headers);
    }
}

auto configure_tcp(options& source, node_model::transport_payload& transport, uri_protocol protocol)
    -> void {
    const auto header = lower(option_text(source, "headerType", "none"));
    if (header == "http") {
        if (protocol == uri_protocol::trojan) {
            fail(parse_error::unsupported_parameter);
        }
        transport = node_model::http_transport{};
        alias(source, "host", "http-opts.headers.Host");
        alias(source, "path", "http-opts.path");
        alias(source, "method", "http-opts.method");
        if (const auto* host = find_option(source, "http-opts.headers.Host")) {
            auto hosts = std::vector<std::string>{};
            read_value(*host, hosts);
            auto headers = json::object();
            if (const auto* previous = find_option(source, "http-opts.headers")) {
                headers = *previous;
            }
            headers.update(json{{"Host", hosts}});
            set_option(source, "http-opts.headers", headers);
        }
        if (!source.values.contains("httpopts.method")) {
            set_option(source, "http-opts.method", "GET");
        }
    } else if (header != "none" && !header.empty()) {
        fail(parse_error::unsupported_parameter);
    }
}

auto select_transport(options& source, node_configuration& configuration, uri_protocol protocol)
    -> void {
    alias(source, "net", "network");
    alias(source, "type", "network");
    const auto network = normalize_key(option_text(source, "network", "tcp"));
    if (protocol != uri_protocol::vless && protocol != uri_protocol::vmess &&
        protocol != uri_protocol::trojan) {
        if (network != "tcp" && !network.empty()) {
            fail(parse_error::unsupported_parameter);
        }
        return;
    }
    auto& transport = configuration.link.transport;
    if (network == "ws" || network == "websocket" || network == "httpupgrade") {
        configure_websocket(source, transport, network);
    } else if (network == "grpc") {
        transport = node_model::grpc_transport{};
        alias(source, "serviceName", "grpc-opts.grpc-service-name");
        alias(source, "service-name", "grpc-opts.grpc-service-name");
        alias(source, "grpc-service-name", "grpc-opts.grpc-service-name");
        alias(source, "path", "grpc-opts.grpc-service-name");
        alias(source, "grpc-user-agent", "grpc-opts.grpc-user-agent");
    } else if (network == "h2" || network == "http") {
        if (protocol == uri_protocol::trojan) {
            fail(parse_error::unsupported_parameter);
        }
        transport = node_model::http2_transport{};
        alias(source, "host", "h2-opts.host");
        alias(source, "path", "h2-opts.path");
    } else if (network == "xhttp" || network == "splithttp") {
        if (protocol != uri_protocol::vless) {
            fail(parse_error::unsupported_parameter);
        }
        transport = node_model::xhttp_transport{};
        alias(source, "path", "xhttp-opts.path");
        alias(source, "host", "xhttp-opts.host");
        alias(source, "mode", "xhttp-opts.mode");
        if (const auto* extra = find_option(source, "extra")) {
            const auto parsed =
                extra->is_string() ? bounded_json(scalar_string(*extra), source.limits) : *extra;
            import_xhttp_extra(source, "xhttp-opts", parsed);
        }
    } else if (network == "kcp" || network == "mkcp" || network == "mekya") {
        if (protocol != uri_protocol::vmess) {
            fail(parse_error::unsupported_parameter);
        }
        if (network == "mekya") {
            transport = node_model::mekya_transport{};
            alias(source, "url", "mekya-opts.url");
        } else {
            transport = node_model::mkcp_transport{};
            alias(source, "seed", "mkcp-opts.seed");
            alias(source, "path", "mkcp-opts.seed");
            alias(source, "headerType", "mkcp-opts.header");
        }
    } else if (network == "tcp" || network == "raw" || network.empty()) {
        configure_tcp(source, transport, protocol);
    } else {
        fail(parse_error::unsupported_parameter);
    }
}

[[nodiscard]] auto plugin_parts(std::string_view text) -> std::vector<std::string> {
    auto parts = std::vector<std::string>{};
    auto current = std::string{};
    auto escaped = false;
    for (const auto character : text) {
        if (escaped) {
            current.push_back(character);
            escaped = false;
        } else if (character == '\\') {
            escaped = true;
        } else if (character == ';') {
            parts.push_back(std::move(current));
            current.clear();
        } else {
            current.push_back(character);
        }
    }
    if (escaped) {
        fail(parse_error::invalid_parameter);
    }
    parts.push_back(std::move(current));
    return parts;
}

auto select_plugin(options& source, node_model::shadowsocks_param& parameter) -> void {
    const auto specification = option_text(source, "plugin");
    if (specification.empty()) {
        return;
    }
    const auto parts = plugin_parts(specification);
    const auto name = lower(trim(parts.front()));
    for (const auto& part : parts | std::views::drop(1)) {
        if (part.empty()) {
            continue;
        }
        const auto equal = part.find('=');
        const auto key = std::string_view{part}.substr(0, equal);
        set_option(source, "plugin-opts." + std::string{key},
                   equal == std::string::npos ? json(true) : json(part.substr(equal + 1)));
    }
    if (name == "obfs" || name == "simple-obfs" || name == "obfs-local") {
        parameter.plugin = node_model::simple_obfs_param{};
        alias(source, "plugin-opts.obfs", "plugin-opts.mode");
        alias(source, "plugin-opts.obfs-host", "plugin-opts.host");
    } else if (name == "v2ray-plugin") {
        parameter.plugin = node_model::v2ray_plugin_param{.mode = "websocket"};
    } else if (name == "gost-plugin") {
        parameter.plugin = node_model::gost_plugin_param{};
    } else if (name == "shadow-tls") {
        parameter.plugin = node_model::shadow_tls_plugin_param{};
    } else if (name == "restls") {
        parameter.plugin = node_model::restls_plugin_param{};
    } else if (name == "jls") {
        parameter.plugin = node_model::jls_plugin_param{};
    } else if (name == "kcptun") {
        parameter.plugin = node_model::kcptun_plugin_param{};
    } else {
        fail(parse_error::unsupported_parameter);
    }
}

auto select_detail(options& source, node_configuration& configuration, uri_protocol protocol)
    -> void {
    switch (protocol) {
    case uri_protocol::vless:
        configuration.detail = node_model::vless_param{};
        alias(source, "id", "uuid");
        alias(source, "public-key", "reality-opts.public-key");
        break;
    case uri_protocol::vmess:
        configuration.detail = node_model::vmess_param{.cipher = "auto"};
        alias(source, "id", "uuid");
        alias(source, "aid", "alter-id");
        alias(source, "scy", "cipher");
        alias(source, "encryption", "cipher");
        alias(source, "public-key", "reality-opts.public-key");
        if (option_text(source, "cipher", "auto").empty()) {
            set_option(source, "cipher", "auto");
        }
        if (option_text(source, "alter-id", "0").empty()) {
            set_option(source, "alter-id", 0);
        }
        break;
    case uri_protocol::shadowsocks:
        configuration.detail = node_model::shadowsocks_param{};
        select_plugin(source, std::get<node_model::shadowsocks_param>(configuration.detail));
        break;
    case uri_protocol::trojan:
        configuration.detail = node_model::trojan_param{};
        alias(source, "public-key", "reality-opts.public-key");
        break;
    case uri_protocol::hysteria2:
        configuration.detail = node_model::hysteria2_param{};
        alias(source, "auth", "password");
        alias(source, "upmbps", "up");
        alias(source, "downmbps", "down");
        alias(source, "mport", "ports");
        alias(source, "server-ports", "ports");
        break;
    case uri_protocol::tuic:
        configuration.detail = node_model::tuic_param{};
        alias(source, "congestion-control", "congestion-controller");
        alias(source, "heartbeat", "heartbeat-interval");
        alias(source, "zero-rtt-handshake", "reduce-rtt");
        break;
    case uri_protocol::wireguard:
        configuration.detail = node_model::wireguard_param{};
        alias(source, "privatekey", "private-key");
        alias(source, "publickey", "public-key");
        alias(source, "peer-public-key", "public-key");
        alias(source, "presharedkey", "pre-shared-key");
        alias(source, "keepalive", "persistent-keepalive");
        alias(source, "persistent-keepalive-interval", "persistent-keepalive");
        alias(source, "local-address", "address");
        break;
    case uri_protocol::anytls:
        configuration.detail = node_model::anytls_param{};
        for (const auto* const key : {"idle-session-check-interval", "idle-session-timeout"}) {
            if (const auto* value = find_option(source, key)) {
                set_option(source, key, duration_count(*value, 1));
            }
        }
        break;
    case uri_protocol::unspecified:
        fail(parse_error::unsupported_protocol);
    }
}

[[nodiscard]] auto canonical_wireguard_key(std::string_view value) -> std::string {
    if (std::ssize(decode_base64(value)) != wireguard_key_bytes) {
        fail(parse_error::invalid_parameter);
    }
    auto result = std::string{value};
    std::ranges::replace(result, '-', '+');
    std::ranges::replace(result, '_', '/');
    constexpr std::size_t base64_block{4};
    while (result.size() % base64_block != 0) {
        result.push_back('=');
    }
    return result;
}

[[nodiscard]] auto canonical_ip(std::string_view value, bool require_prefix = false)
    -> std::pair<std::string, bool> {
    const auto slash = value.find('/');
    auto host = value.substr(0, slash);
    if (host.starts_with('[') && host.ends_with(']')) {
        host.remove_prefix(1);
        host.remove_suffix(1);
    }
    const auto ipv6 = host.find(':') != std::string_view::npos;
    if (!(ipv6 ? valid_ipv6(host) : valid_ipv4(host))) {
        fail(parse_error::invalid_parameter);
    }
    constexpr std::int64_t ipv4_prefix{32};
    constexpr std::int64_t ipv6_prefix{128};
    const auto maximum = ipv6 ? ipv6_prefix : ipv4_prefix;
    auto prefix = maximum;
    if (slash != std::string_view::npos) {
        prefix = integer(value.substr(slash + 1));
    } else if (require_prefix) {
        fail(parse_error::invalid_parameter);
    }
    if (prefix < 0 || prefix > maximum) {
        fail(parse_error::invalid_parameter);
    }
    return {std::string{host} + '/' + std::to_string(prefix), ipv6};
}

auto prepare_wireguard(options& source) -> void {
    const auto* addresses = find_option(source, "address");
    if (addresses == nullptr) {
        return;
    }
    auto entries = std::vector<std::string>{};
    read_value(*addresses, entries);
    for (const auto& entry : entries) {
        const auto [address, ipv6] = canonical_ip(entry);
        const auto* const key = ipv6 ? "ipv6" : "ip";
        if (source.values.contains(key)) {
            fail(parse_error::invalid_parameter);
        }
        set_option(source, key, address);
    }
}

auto prepare_wireguard_prefixes(options& source) -> void {
    for (const auto* const key : {"ip", "ipv6"}) {
        const auto* prefix = find_option(source, std::string{key} + "-cidr");
        if (prefix == nullptr) {
            continue;
        }
        const auto address = option_text(source, key);
        if (address.empty() || address.find('/') != std::string::npos) {
            fail(parse_error::invalid_parameter);
        }
        set_option(source, key, address + '/' + scalar_string(*prefix));
    }
}

auto validate_wireguard(node_model::wireguard_param& parameter) -> void {
    if (parameter.private_key.empty() ||
        (parameter.public_key.empty() && parameter.peers.empty()) ||
        (parameter.ipv4_address.empty() && parameter.ipv6_address.empty())) {
        fail(parse_error::missing_credential);
    }
    parameter.private_key = canonical_wireguard_key(parameter.private_key);
    if (!parameter.public_key.empty()) {
        parameter.public_key = canonical_wireguard_key(parameter.public_key);
    }
    if (!parameter.pre_shared_key.empty()) {
        parameter.pre_shared_key = canonical_wireguard_key(parameter.pre_shared_key);
    }
    if (!parameter.reserved.empty() && std::ssize(parameter.reserved) != wireguard_reserved_bytes) {
        fail(parse_error::invalid_parameter);
    }
    if (!parameter.ipv4_address.empty()) {
        const auto [address, ipv6] = canonical_ip(parameter.ipv4_address);
        if (ipv6) {
            fail(parse_error::invalid_parameter);
        }
        parameter.ipv4_address = address;
    }
    if (!parameter.ipv6_address.empty()) {
        const auto [address, ipv6] = canonical_ip(parameter.ipv6_address);
        if (!ipv6) {
            fail(parse_error::invalid_parameter);
        }
        parameter.ipv6_address = address;
    }
    for (auto& address : parameter.allowed_ips) {
        address = canonical_ip(address, true).first;
    }
    for (auto& peer : parameter.peers) {
        static_cast<void>(endpoint(peer.server.find(':') == std::string::npos
                                       ? peer.server + ':' + std::to_string(peer.port)
                                       : '[' + peer.server + "]:" + std::to_string(peer.port),
                                   0));
        peer.public_key = canonical_wireguard_key(peer.public_key);
        if (!peer.pre_shared_key.empty()) {
            peer.pre_shared_key = canonical_wireguard_key(peer.pre_shared_key);
        }
        if (!peer.reserved.empty() && std::ssize(peer.reserved) != wireguard_reserved_bytes) {
            fail(parse_error::invalid_parameter);
        }
        for (auto& address : peer.allowed_ips) {
            address = canonical_ip(address, true).first;
        }
    }
}

[[nodiscard]] auto valid_uuid(std::string_view value) noexcept -> bool {
    constexpr std::size_t uuid_size{36};
    constexpr auto hyphens = std::to_array<std::int64_t>({8, 13, 18, 23});
    if (value.size() != uuid_size) {
        return false;
    }
    auto index = std::int64_t{};
    for (const auto character : value) {
        const auto hyphen = std::ranges::find(hyphens, index) != hyphens.end();
        if ((hyphen && character != '-') || (!hyphen && hex_digit(character) < 0)) {
            return false;
        }
        ++index;
    }
    return true;
}

auto validate_vless(node_model::vless_param& parameter) -> void {
    if (parameter.uuid.empty()) {
        fail(parse_error::missing_credential);
    }
    if (parameter.flow == "xtls-rprx-vision-udp443") {
        parameter.flow = "xtls-rprx-vision";
    }
    if (!parameter.flow.empty() && parameter.flow != "xtls-rprx-vision") {
        fail(parse_error::unsupported_parameter);
    }
}

auto validate_vmess(node_model::vmess_param& parameter) -> void {
    if (parameter.uuid.empty()) {
        fail(parse_error::missing_credential);
    }
    parameter.cipher = lower(parameter.cipher);
    if (parameter.cipher == "chacha20-ietf-poly1305") {
        parameter.cipher = "chacha20-poly1305";
    } else if (parameter.cipher == "gcm") {
        parameter.cipher = "aes-128-gcm";
    }
    if (parameter.cipher != "auto" && parameter.cipher != "none" &&
        parameter.cipher != "aes-128-gcm" && parameter.cipher != "chacha20-poly1305") {
        fail(parse_error::unsupported_parameter);
    }
}

auto validate_hysteria2(node_model::hysteria2_param& parameter) -> void {
    if (parameter.password.empty()) {
        fail(parse_error::missing_credential);
    }
    if (parameter.obfs.empty() && !parameter.obfs_password.empty()) {
        parameter.obfs = "salamander";
    }
    if (!parameter.obfs.empty() && parameter.obfs != "salamander" && parameter.obfs != "gecko") {
        fail(parse_error::unsupported_parameter);
    }
    if (!parameter.obfs.empty() && parameter.obfs_password.empty()) {
        fail(parse_error::missing_credential);
    }
}

auto validate_detail(node_configuration& configuration, uri_protocol protocol) -> void {
    switch (protocol) {
    case uri_protocol::vless:
        validate_vless(std::get<node_model::vless_param>(configuration.detail));
        break;
    case uri_protocol::vmess:
        validate_vmess(std::get<node_model::vmess_param>(configuration.detail));
        break;
    case uri_protocol::shadowsocks: {
        const auto& parameter = std::get<node_model::shadowsocks_param>(configuration.detail);
        if (parameter.cipher.empty() || parameter.password.empty()) {
            fail(parse_error::missing_credential);
        }
        break;
    }
    case uri_protocol::trojan:
        if (std::get<node_model::trojan_param>(configuration.detail).password.empty()) {
            fail(parse_error::missing_credential);
        }
        break;
    case uri_protocol::hysteria2:
        validate_hysteria2(std::get<node_model::hysteria2_param>(configuration.detail));
        break;
    case uri_protocol::tuic: {
        const auto& parameter = std::get<node_model::tuic_param>(configuration.detail);
        if (!valid_uuid(parameter.uuid)) {
            fail(parse_error::invalid_parameter);
        }
        const auto& mode = configuration.link.quic.udp_relay_mode;
        if (!mode.empty() && mode != "native" && mode != "quic") {
            fail(parse_error::unsupported_parameter);
        }
        break;
    }
    case uri_protocol::wireguard:
        validate_wireguard(std::get<node_model::wireguard_param>(configuration.detail));
        break;
    case uri_protocol::anytls:
        if (std::get<node_model::anytls_param>(configuration.detail).password.empty()) {
            fail(parse_error::missing_credential);
        }
        break;
    case uri_protocol::unspecified:
        fail(parse_error::unsupported_protocol);
    }
    if (auto* websocket =
            std::get_if<node_model::websocket_transport>(&configuration.link.transport)) {
        if (websocket->max_early_data > 0 && websocket->early_data_header_name.empty()) {
            websocket->early_data_header_name = "Sec-WebSocket-Protocol";
        }
    }
    auto& security = configuration.link.security;
    if (security.reality.has_value() && security.reality->public_key.empty()) {
        fail(parse_error::missing_credential);
    }
    if (security.reality.has_value()) {
        if (security.tls.enabled == false) {
            fail(parse_error::invalid_parameter);
        }
        security.tls.enabled = true;
    }
}

[[nodiscard]] auto collect_ignored(const options& source) -> std::vector<std::string> {
    auto result = std::vector<std::string>{};
    for (const auto& [key, entry] : source.values) {
        if (entry.consumed || entry.value.is_object()) {
            continue;
        }
        auto ancestor = key;
        auto handled = false;
        while (ancestor.find('.') != std::string::npos) {
            ancestor.resize(ancestor.rfind('.'));
            const auto found = source.values.find(ancestor);
            if (found != source.values.end() && found->second.consumed) {
                handled = true;
                break;
            }
        }
        if (!handled) {
            result.push_back(key.substr(0, static_cast<std::size_t>(maximum_warning_key)));
        }
    }
    return result;
}

[[nodiscard]] auto valid_limits(const parse_limits& limits) noexcept -> bool {
    return limits.max_input_bytes > 0 && limits.max_line_bytes > 0 && limits.max_nodes > 0 &&
           limits.max_lines > 0 && limits.max_parameters > 0 && limits.max_json_depth > 0 &&
           limits.max_yaml_nodes > 0 && limits.max_expanded_values > 0;
}

[[nodiscard]] constexpr auto remove_bom(std::string_view input) noexcept -> std::string_view {
    constexpr std::string_view utf8_bom{"\xef\xbb\xbf"};
    input = trim(input);
    if (input.starts_with(utf8_bom)) {
        input.remove_prefix(utf8_bom.size());
    }
    return trim(input);
}

[[nodiscard]] constexpr auto has_scheme(std::string_view input) noexcept -> bool {
    return input.find("://") != std::string_view::npos;
}

struct uri_parts { // NOLINT(bugprone-exception-escape)
    uri_protocol protocol{uri_protocol::unspecified};
    node_model::remote_endpoint remote{};
    std::string fragment{};
    options fields{};
};

auto set_credential(uri_parts& parts, std::string_view userinfo, bool decoded_legacy) -> void {
    const auto auth = decoded_legacy ? std::string{userinfo} : percent_decode(userinfo);
    switch (parts.protocol) {
    case uri_protocol::shadowsocks: {
        const auto decoded = auth.find(':') == std::string::npos ? decode_base64(auth) : auth;
        const auto colon = decoded.find(':');
        if (colon == std::string::npos) {
            fail(parse_error::missing_credential);
        }
        set_option(parts.fields, "cipher", decoded.substr(0, colon));
        set_option(parts.fields, "password", decoded.substr(colon + 1));
        break;
    }
    case uri_protocol::vless:
        set_option(parts.fields, "uuid", auth);
        break;
    case uri_protocol::vmess: {
        const auto colon = auth.find(':');
        if (colon == std::string::npos) {
            set_option(parts.fields, "uuid", auth);
        } else {
            set_option(parts.fields, "cipher", auth.substr(0, colon));
            set_option(parts.fields, "uuid", auth.substr(colon + 1));
        }
        break;
    }
    case uri_protocol::tuic: {
        const auto colon = auth.find(':');
        if (colon == std::string::npos) {
            fail(parse_error::missing_credential);
        }
        set_option(parts.fields, "uuid", auth.substr(0, colon));
        set_option(parts.fields, "password", auth.substr(colon + 1));
        break;
    }
    case uri_protocol::wireguard:
        set_option(parts.fields, "private-key", auth);
        break;
    case uri_protocol::trojan:
    case uri_protocol::hysteria2:
    case uri_protocol::anytls:
        set_option(parts.fields, "password", auth);
        break;
    case uri_protocol::unspecified:
        fail(parse_error::unsupported_protocol);
    }
}

[[nodiscard]] auto hopping_first_port(std::string_view value) -> std::int64_t {
    auto canonical = std::string{value};
    std::ranges::replace(canonical, ';', ',');
    auto first_port = std::int64_t{};
    for (const auto& range : split(canonical, ',')) {
        const auto ends = split(range, '-');
        if (ends.size() > 2) {
            fail(parse_error::invalid_endpoint);
        }
        const auto first = integer(ends.front());
        const auto last = integer(ends.back());
        if (first <= 0 || last > maximum_port || first > last) {
            fail(parse_error::invalid_endpoint);
        }
        if (first_port == 0) {
            first_port = first;
        }
    }
    return first_port;
}

auto parse_vmess_json(uri_parts& parts, std::string_view encoded) -> void {
    const auto decoded = decode_base64(percent_decode(encoded));
    const auto object = bounded_json(decoded, parts.fields.limits);
    if (!object.is_object()) {
        fail(parse_error::invalid_uri);
    }
    for (const auto& [key, value] : object.items()) {
        if (value.is_null()) {
            continue;
        }
        if (normalize_key(key) == "type") {
            set_option(parts.fields, "headerType", value);
        } else {
            set_option(parts.fields, key, value);
        }
    }
    alias(parts.fields, "ps", "remarks");
    alias(parts.fields, "add", "server");
    static_cast<void>(find_option(parts.fields, "v"));
    const auto host = option_text(parts.fields, "server");
    const auto port = option_text(parts.fields, "port");
    const auto authority =
        host.find(':') == std::string::npos ? host + ':' + port : '[' + host + "]:" + port;
    parts.remote = endpoint(authority, 0);
    const auto mode = option_text(parts.fields, "network", option_text(parts.fields, "net", "tcp"));
    if (normalize_key(mode) != "tcp" && normalize_key(mode) != "raw" &&
        normalize_key(mode) != "kcp" && normalize_key(mode) != "mkcp") {
        static_cast<void>(find_option(parts.fields, "headerType"));
    }
}

[[nodiscard]] auto parse_remote(uri_parts& parts, std::string_view authority)
    -> node_model::remote_endpoint {
    const auto default_port = parts.protocol == uri_protocol::wireguard ? wireguard_port : tls_port;
    const auto require_port = parts.protocol == uri_protocol::shadowsocks ||
                              parts.protocol == uri_protocol::vless ||
                              parts.protocol == uri_protocol::vmess;
    if (parts.protocol == uri_protocol::hysteria2) {
        const auto port_colon = authority.rfind(':');
        if (port_colon != std::string_view::npos &&
            (!authority.starts_with('[') || port_colon > authority.find(']'))) {
            const auto ports = authority.substr(port_colon + 1);
            if (ports.find_first_of(",;-") != std::string_view::npos) {
                const auto port = hopping_first_port(ports);
                set_option(parts.fields, "ports", std::string{ports});
                const auto adjusted_authority =
                    std::string{authority.substr(0, port_colon + 1)} + std::to_string(port);
                return endpoint(adjusted_authority, default_port);
            }
        }
    }
    return endpoint(authority, require_port ? 0 : default_port);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto parse_parts(std::string_view input, const parse_limits& limits) -> uri_parts {
    const auto separator = input.find("://");
    if (separator == std::string_view::npos || separator == 0) {
        fail(parse_error::invalid_uri);
    }
    auto parts = uri_parts{};
    parts.fields.limits = limits;
    parts.protocol = parse_protocol(input.substr(0, separator));
    const auto realm = lower(input.substr(0, separator)).ends_with("+realm");
    auto body = input.substr(separator + 3);
    const auto hash = body.find('#');
    if (hash != std::string_view::npos) {
        parts.fragment = percent_decode(body.substr(hash + 1));
        body = body.substr(0, hash);
    }
    if (parts.protocol == uri_protocol::vmess && body.find('@') == std::string_view::npos) {
        parse_vmess_json(parts, body);
        return parts;
    }
    const auto question = body.find('?');
    if (question != std::string_view::npos) {
        parse_query(parts.fields, body.substr(question + 1));
        body = body.substr(0, question);
    }
    auto legacy_ss = std::string{};
    if (parts.protocol == uri_protocol::shadowsocks && body.find('@') == std::string_view::npos) {
        legacy_ss = decode_base64(percent_decode(body));
        body = legacy_ss;
    }
    const auto at_sign = body.rfind('@');
    auto authority = at_sign == std::string_view::npos ? body : body.substr(at_sign + 1);
    if (realm) {
        const auto slash = authority.find('/');
        if (slash == std::string_view::npos || slash + 1 == authority.size()) {
            fail(parse_error::missing_credential);
        }
        set_option(parts.fields, "realm-opts.realm-id",
                   percent_decode(authority.substr(slash + 1)));
        authority = authority.substr(0, slash);
    }
    if (authority.ends_with('/')) {
        authority.remove_suffix(1);
    }
    if (authority.find('/') != std::string_view::npos) {
        fail(parse_error::invalid_uri);
    }
    if (at_sign != std::string_view::npos) {
        if (realm) {
            const auto auth = percent_decode(body.substr(0, at_sign));
            if (auth.empty()) {
                fail(parse_error::missing_credential);
            }
            set_option(parts.fields, "realm-opts.token", auth);
            alias(parts.fields, "auth", "password");
        } else {
            set_credential(parts, body.substr(0, at_sign), !legacy_ss.empty());
        }
    }
    parts.remote = parse_remote(parts, authority);
    if (realm) {
        set_option(parts.fields, "realm-opts.enable", true);
        set_option(parts.fields, "realm-opts.server-url", "https://" + std::string{authority});
        alias(parts.fields, "stun", "realm-opts.stun-servers");
    }
    return parts;
}

[[nodiscard]] auto assemble(uri_parts parts, bool uri_defaults = true) -> node_parse_result {
    // 各输入格式先归一为 options，再共用协议/传输绑定和校验。URI 的隐含默认值
    // 只在 uri_defaults 开启时补入，避免把分享链接习惯错误套到完整配置文档上。
    auto configuration = node_configuration{};
    auto& source = parts.fields;
    if (uri_defaults) {
        configuration.link.udp = true;
        normalize_common(source, parts.protocol);
    }
    if (parts.protocol == uri_protocol::trojan || parts.protocol == uri_protocol::hysteria2 ||
        parts.protocol == uri_protocol::tuic || parts.protocol == uri_protocol::anytls) {
        if (const auto* tls = find_option(source, "tls")) {
            auto enabled = false;
            read_value(*tls, enabled);
            if (!enabled) {
                fail(parse_error::unsupported_parameter);
            }
        }
        configuration.link.security.tls.enabled = true;
    }
    select_detail(source, configuration, parts.protocol);
    select_transport(source, configuration, parts.protocol);
    if (parts.protocol == uri_protocol::wireguard) {
        prepare_wireguard(source);
        prepare_wireguard_prefixes(source);
    }
    bind_kernel_fields(source, configuration, parts.protocol);
    if (lower(option_text(source, "security")) == "reality" &&
        !configuration.link.security.reality.has_value()) {
        fail(parse_error::missing_credential);
    }
    if (parts.protocol == uri_protocol::hysteria2 &&
        !configuration.link.port_hopping.ports.empty()) {
        static_cast<void>(hopping_first_port(configuration.link.port_hopping.ports));
        std::ranges::replace(configuration.link.port_hopping.ports, ';', ',');
    }
    validate_detail(configuration, parts.protocol);
    alias(source, "name", "remarks");
    alias(source, "tag", "remarks");
    alias(source, "ps", "remarks");
    auto name = parts.fragment;
    if (name.empty()) {
        name = option_text(source, "remarks", option_text(source, "remark"));
    }
    if (name.empty()) {
        name = parts.remote.host + ':' + std::to_string(parts.remote.port);
    }
    auto result = node_parse_result{};
    result.ignored_parameters = collect_ignored(source);
    result.node.emplace(node_model::unified_node{
        .core = {.name = std::move(name),
                 .protocol = model_protocol(parts.protocol),
                 .remote = std::move(parts.remote)},
        .configuration = std::make_unique<const node_configuration>(std::move(configuration))});
    return result;
}

auto parse_lines(std::string_view text, const parse_limits& limits, subscription_result& result)
    -> void {
    // 行级语法错误允许继续导入，内存不足则终止整份输入。诊断保留原始行号，
    // 包括空行和注释行，便于用户回到订阅原文定位问题。
    auto line_number = std::int64_t{};
    while (!text.empty()) {
        ++line_number;
        if (line_number > limits.max_lines) {
            fail(parse_error::resource_limit);
        }
        const auto end = text.find_first_of("\r\n");
        const auto line = remove_bom(text.substr(0, end));
        if (end == std::string_view::npos) {
            text = {};
        } else {
            const auto crlf =
                text.at(end) == '\r' && end + 1 < text.size() && text.at(end + 1) == '\n';
            text.remove_prefix(end + (crlf ? 2 : 1));
        }
        if (line.empty() || line.starts_with('#') || line.starts_with("//")) {
            continue;
        }
        if (std::ssize(result.nodes) >= limits.max_nodes) {
            fail(parse_error::resource_limit);
        }
        auto parsed = parse_uri(line, limits);
        if (parsed.error != parse_error::none || !parsed.ignored_parameters.empty()) {
            result.diagnostics.push_back(
                {.line = line_number,
                 .error = parsed.error,
                 .ignored_parameters = std::move(parsed.ignored_parameters)});
        }
        if (parsed.node.has_value()) {
            result.nodes.push_back(std::move(*parsed.node));
        }
        if (parsed.error == parse_error::allocation_failure) {
            fail(parse_error::allocation_failure);
        }
    }
}

#include "node_input_document.inc"
#include "node_input_singbox.inc"
#include "node_input_wireguard.inc"
#include "node_input_yaml.inc"

// NOLINTNEXTLINE(misc-no-recursion)
auto dispatch_subscription(std::string_view input, const parse_limits& limits,
                           subscription_result& result, std::int64_t unwraps) -> void {
    // 先识别明确的文档/链接特征，最后才尝试 Base64，防止普通文本被误解码。
    // 最多剥两层包装；递归深度是协议兼容范围，不允许无限“猜到能解析为止”。
    input = remove_bom(input);
    input = skip_json_preamble(input);
    if (input.empty()) {
        return;
    }
    if (starts_with_uri(input)) {
        parse_lines(input, limits, result);
    } else if (has_yaml_root_key(input, limits)) {
        result.format = subscription_format::clash_yaml;
        parse_clash_yaml(input, limits, result);
    } else if (has_wireguard_section(input, limits)) {
        result.format = subscription_format::wireguard_conf;
        parse_wireguard_conf(input, limits, result);
    } else if (input.starts_with('{') || input.starts_with('[')) {
        parse_json_document(input, limits, result);
    } else if (has_scheme(input) || input.starts_with('#') || input.starts_with("//")) {
        parse_lines(input, limits, result);
    } else {
        if (unwraps >= 2) {
            fail(parse_error::invalid_document);
        }
        const auto decoded = decode_base64(input, true);
        result.encoding =
            unwraps == 0 ? subscription_encoding::base64 : subscription_encoding::double_base64;
        dispatch_subscription(decoded, limits, result, unwraps + 1);
    }
}

// NOLINTEND(llvm-prefer-static-over-anonymous-namespace, fuchsia-default-arguments-calls,

} // namespace

namespace material {
auto extract_clash_material(std::string_view input, const parse_limits& limits) noexcept
    -> clash_material {
    try {
        if (!valid_limits(limits) || std::ssize(input) > limits.max_input_bytes) {
            return {.error = parse_error::resource_limit};
        }
        return read_clash_material(input, limits);
    } catch (const input_failure& failure) {
        return {.error = failure.error};
    } catch (const std::bad_alloc&) {
        return {.error = parse_error::allocation_failure};
    } catch (const std::exception&) {
        return {.error = parse_error::invalid_document};
    } catch (...) {
        return {.error = parse_error::unexpected_failure};
    }
}
} // namespace material

auto parse_uri(std::string_view input, const parse_limits& limits) noexcept -> node_parse_result {
    try {
        if (!valid_limits(limits) || std::ssize(input) > limits.max_line_bytes ||
            std::ssize(input) > limits.max_input_bytes) {
            return {.error = parse_error::resource_limit};
        }
        input = remove_bom(input);
        if (input.empty()) {
            return {.error = parse_error::invalid_uri};
        }
        return assemble(parse_parts(input, limits), true);
    } catch (const input_failure& failure) {
        return {.error = failure.error};
    } catch (const json::exception&) {
        return {.error = parse_error::invalid_parameter};
    } catch (const std::bad_alloc&) {
        return {.error = parse_error::allocation_failure};
    } catch (const std::exception&) {
        return {.error = parse_error::unexpected_failure};
    } catch (...) {
        return {.error = parse_error::unexpected_failure};
    }
}

auto parse_subscription(std::string_view input, const parse_limits& limits) noexcept
    -> subscription_result {
    // 对外统一成显式错误值；内部解析器可以抛出带类型的失败来及时退出深层解析。
    // 出错前得到的节点仍可能留在 result 中，因此成功判据始终是 error == none。
    auto result = subscription_result{};
    try {
        if (!valid_limits(limits) || std::ssize(input) > limits.max_input_bytes) {
            result.error = parse_error::resource_limit;
            return result;
        }
        dispatch_subscription(input, limits, result, 0);
    } catch (const input_failure& failure) {
        result.error = failure.error;
    } catch (const json::exception&) {
        result.error = parse_error::invalid_document;
    } catch (const std::bad_alloc&) {
        result.error = parse_error::allocation_failure;
    } catch (const std::exception&) {
        result.error = parse_error::unexpected_failure;
    } catch (...) {
        result.error = parse_error::unexpected_failure;
    }
    return result;
}

} // namespace node_input
