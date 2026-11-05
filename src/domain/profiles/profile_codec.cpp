#include <algorithm>
#include <array>
#include <bit>    // IWYU pragma: keep; private SAX adapter sizes its constexpr field lookup table.
#include <bitset> // IWYU pragma: keep; private SAX adapter uses per-object key bitmaps.
#include <blank/nodes/unified_node.hpp>
#include <blank/profiles/profile_codec.hpp>
#include <blank/profiles/profile_types.hpp>
#include <charconv> // IWYU pragma: keep; private cache writer uses std::to_chars.
#include <chrono>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace profile_storage::detail {
namespace {

// NOLINTBEGIN(llvm-prefer-static-over-anonymous-namespace,altera-struct-pack-align,readability-redundant-member-init,altera-unroll-loops,fuchsia-default-arguments-calls)
using nlohmann::json;

struct codec_failure final : std::exception {};

[[noreturn]] auto invalid() -> void {
    throw codec_failure{};
}

template <typename Value> struct record_traits;
template <typename Value> struct enum_traits;
template <typename Value> struct variant_tag;

#include "profile_codec_fields.inc"

enum class value_kind : std::int8_t {
    scalar,
    optional,
    pointer,
    vector,
    array,
    pair,
    variant,
    duration,
};
template <typename Value> struct value_traits {
    [[maybe_unused]] static constexpr auto kind = value_kind::scalar;
};
template <typename Value> struct value_traits<std::optional<Value>> {
    [[maybe_unused]] static constexpr auto kind = value_kind::optional;
    using element_type = Value;
};
template <typename Value> struct value_traits<std::unique_ptr<Value>> {
    [[maybe_unused]] static constexpr auto kind = value_kind::pointer;
    using element_type = std::remove_const_t<Value>;
};
template <typename Value> struct value_traits<std::vector<Value>> {
    [[maybe_unused]] static constexpr auto kind = value_kind::vector;
    using element_type = Value;
};
template <typename Value, std::size_t Count> struct value_traits<std::array<Value, Count>> {
    [[maybe_unused]] static constexpr auto kind = value_kind::array;
    using element_type = Value;
};
template <typename First, typename Second> struct value_traits<std::pair<First, Second>> {
    [[maybe_unused]] static constexpr auto kind = value_kind::pair;
    using first_type = First;
    using second_type = Second;
};
template <typename... Values> struct value_traits<std::variant<Values...>> {
    [[maybe_unused]] static constexpr auto kind = value_kind::variant;
};
template <typename Rep, typename Period> struct value_traits<std::chrono::duration<Rep, Period>> {
    [[maybe_unused]] static constexpr auto kind = value_kind::duration;
};

template <typename Value>
concept cache_value = std::is_object_v<Value>;

template <cache_value Value> [[nodiscard]] constexpr auto scalar_encoding() noexcept -> bool {
    if constexpr (value_traits<Value>::kind == value_kind::optional) {
        return scalar_encoding<typename value_traits<Value>::element_type>();
    } else {
        return std::integral<Value> || std::same_as<Value, std::byte> ||
               std::same_as<Value, std::monostate> ||
               value_traits<Value>::kind == value_kind::duration;
    }
}

template <typename Value>
concept scalar_cache_value = scalar_encoding<Value>();

template <scalar_cache_value Value> [[nodiscard]] auto encode(const Value& value) noexcept -> json {
    if constexpr (std::same_as<Value, std::monostate>) {
        return nullptr;
    } else if constexpr (std::same_as<Value, std::byte> || std::same_as<Value, char>) {
        return static_cast<unsigned char>(value);
    } else if constexpr (value_traits<Value>::kind == value_kind::duration) {
        return value.count();
    } else if constexpr (value_traits<Value>::kind == value_kind::optional) {
        return value ? encode(*value) : json(nullptr);
    } else {
        return json(value);
    }
}

template <cache_value Value>
    requires(!scalar_cache_value<Value>)
[[nodiscard]] auto encode(const Value& value) -> json;

template <cache_value Value> [[nodiscard]] auto encode_enum(Value value) -> json {
    const auto found = std::ranges::find(enum_traits<Value>::values, value,
                                         &std::pair<std::string_view, Value>::second);
    if (found != enum_traits<Value>::values.end()) {
        return std::string{found->first};
    }
    invalid();
}

template <cache_value Value> [[nodiscard]] auto encode_sequence(const Value& value) -> json {
    auto result = json::array();
    for (const auto& item : value) {
        result.push_back(encode(item));
    }
    return result;
}

template <cache_value Value> [[nodiscard]] auto decode(const json& value) -> Value;

auto object_size(const json& value, std::size_t count) -> void {
    if (!value.is_object() || value.size() != count) {
        invalid();
    }
}

[[nodiscard]] auto integer(const json& value) -> std::int64_t {
    if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        if (std::cmp_greater(number, std::numeric_limits<std::int64_t>::max())) {
            invalid();
        }
        return static_cast<std::int64_t>(number);
    }
    if (!value.is_number_integer()) {
        invalid();
    }
    return value.get<std::int64_t>();
}

template <std::integral Value> [[nodiscard]] auto bounded_integer(const json& value) -> Value {
    const auto number = integer(value);
    if (!std::in_range<Value>(number)) {
        invalid();
    }
    return static_cast<Value>(number);
}

template <cache_value Variant, std::size_t Index>
[[nodiscard]] auto decode_variant(const json& value) -> Variant {
    if constexpr (Index == std::variant_size_v<Variant>) {
        invalid();
    } else {
        using alternative = std::variant_alternative_t<Index, Variant>;
        if (decode<std::string>(value.at("type")) == variant_tag<alternative>::name) {
            return Variant{std::in_place_index<Index>, decode<alternative>(value.at("value"))};
        }
        return decode_variant<Variant, Index + 1>(value);
    }
}

template <cache_value Value>
    requires(!scalar_cache_value<Value>)
[[nodiscard]] auto encode(const Value& value) -> json {
    constexpr auto kind = value_traits<Value>::kind;
    if constexpr (std::same_as<Value, std::string>) {
        return json(value);
    } else if constexpr (std::floating_point<Value>) {
        if (!std::isfinite(value)) {
            invalid();
        }
        return json(value);
    } else if constexpr (std::is_enum_v<Value>) {
        return encode_enum(value);
    } else if constexpr (kind == value_kind::optional || kind == value_kind::pointer) {
        return value ? encode(*value) : json(nullptr);
    } else if constexpr (std::same_as<Value, std::chrono::system_clock::time_point>) {
        using period = Value::duration::period;
        return json{
            {"ticks", value.time_since_epoch().count()},
            {"period_num", period::num},
            {"period_den", period::den},
        };
    } else if constexpr (kind == value_kind::variant) {
        if (value.valueless_by_exception()) {
            invalid();
        }
        return std::visit(
            []<cache_value Alternative>(const Alternative& item) -> json {
                return json{{"type", variant_tag<Alternative>::name}, {"value", encode(item)}};
            },
            value);
    } else if constexpr (kind == value_kind::pair) {
        return json::array({encode(value.first), encode(value.second)});
    } else if constexpr (kind == value_kind::vector || kind == value_kind::array) {
        return encode_sequence(value);
    } else {
        auto result = json::object();
        std::apply(
            [&result, &value](const auto&... fields) -> void {
                (static_cast<void>(result.emplace(fields.first, encode(value.*fields.second))),
                 ...);
            },
            record_traits<Value>::members);
        return result;
    }
}

template <cache_value Value> [[nodiscard]] auto decode_enum(const json& value) -> Value {
    const auto name = decode<std::string>(value);
    const auto found = std::ranges::find(enum_traits<Value>::values, name,
                                         &std::pair<std::string_view, Value>::first);
    if (found != enum_traits<Value>::values.end()) {
        return found->second;
    }
    invalid();
}

template <cache_value Value> [[nodiscard]] auto decode_atom(const json& value) -> Value {
    constexpr auto kind = value_traits<Value>::kind;
    if constexpr (std::same_as<Value, std::monostate>) {
        if (!value.is_null()) {
            invalid();
        }
        return {};
    } else if constexpr (std::same_as<Value, std::byte> || std::same_as<Value, char>) {
        return static_cast<Value>(bounded_integer<unsigned char>(value));
    } else if constexpr (std::same_as<Value, bool>) {
        if (!value.is_boolean()) {
            invalid();
        }
        return value.get<bool>();
    } else if constexpr (std::integral<Value>) {
        return bounded_integer<Value>(value);
    } else if constexpr (std::floating_point<Value>) {
        if (!value.is_number()) {
            invalid();
        }
        const auto result = value.get<Value>();
        if (!std::isfinite(result)) {
            invalid();
        }
        return result;
    } else if constexpr (std::same_as<Value, std::string>) {
        if (!value.is_string()) {
            invalid();
        }
        return value.get<std::string>();
    } else if constexpr (kind == value_kind::duration) {
        return Value{bounded_integer<typename Value::rep>(value)};
    } else if constexpr (std::same_as<Value, std::chrono::system_clock::time_point>) {
        using period = std::chrono::system_clock::period;
        object_size(value, 3);
        if (integer(value.at("period_num")) != period::num ||
            integer(value.at("period_den")) != period::den) {
            throw storage_failure{storage_error::unsupported_version};
        }
        return Value{
            std::chrono::system_clock::duration{
                bounded_integer<std::chrono::system_clock::rep>(value.at("ticks")),
            },
        };
    } else if constexpr (std::is_enum_v<Value>) {
        return decode_enum<Value>(value);
    }
}

template <cache_value Value> [[nodiscard]] auto decode_sequence(const json& value) -> Value {
    if (!value.is_array()) {
        invalid();
    }
    auto result = Value{};
    using element = value_traits<Value>::element_type;
    if constexpr (value_traits<Value>::kind == value_kind::vector) {
        result.reserve(value.size());
        for (const auto& item : value) {
            result.push_back(decode<element>(item));
        }
    } else {
        if (value.size() != result.size()) {
            invalid();
        }
        auto current = value.begin();
        static_cast<void>(std::ranges::generate(
            result, [&current]() -> element { return decode<element>(*current++); }));
    }
    return result;
}

template <cache_value Value> [[nodiscard]] auto decode(const json& value) -> Value {
    constexpr auto kind = value_traits<Value>::kind;
    if constexpr (kind == value_kind::optional || kind == value_kind::pointer) {
        if (value.is_null()) {
            return {};
        }
        using element = value_traits<Value>::element_type;
        if constexpr (kind == value_kind::pointer) {
            return std::make_unique<element>(decode<element>(value));
        } else {
            return decode<element>(value);
        }
    } else if constexpr (kind == value_kind::variant) {
        object_size(value, 2);
        return decode_variant<Value, 0>(value);
    } else if constexpr (kind == value_kind::pair) {
        if (!value.is_array() || value.size() != 2) {
            invalid();
        }
        return Value{
            decode<typename value_traits<Value>::first_type>(value.at(0)),
            decode<typename value_traits<Value>::second_type>(value.at(1)),
        };
    } else if constexpr (kind == value_kind::vector || kind == value_kind::array) {
        return decode_sequence<Value>(value);
    } else if constexpr (requires { record_traits<Value>::members; }) {
        auto result = Value{};
        object_size(value, std::tuple_size_v<decltype(record_traits<Value>::members)>);
        std::apply(
            [&result, &value](const auto&... fields) -> void {
                ((result.*fields.second =
                      decode<std::remove_cvref_t<decltype(result.*fields.second)>>(
                          value.at(fields.first))),
                 ...);
            },
            record_traits<Value>::members);
        return result;
    } else {
        return decode_atom<Value>(value);
    }
}

#include "profile_codec_nodes.inc"

[[nodiscard]] auto parse_document(std::string_view text) -> json {
    constexpr auto max_depth = 64;
    auto object_keys = std::vector<std::unordered_set<std::string>>{};
    return json::parse(text,
                       [&object_keys](int depth, json::parse_event_t event, json& parsed) -> bool {
                           if (depth > max_depth) {
                               throw storage_failure{storage_error::resource_limit};
                           }
                           if (event == json::parse_event_t::object_start) {
                               object_keys.emplace_back();
                           } else if (event == json::parse_event_t::key) {
                               if (object_keys.empty() ||
                                   !object_keys.back().insert(parsed.get<std::string>()).second) {
                                   invalid();
                               }
                           } else if (event == json::parse_event_t::object_end) {
                               object_keys.pop_back();
                           }
                           return true;
                       });
}

[[nodiscard]] auto document_array(json& document, std::string_view key, std::int64_t limit)
    -> json& {
    if (integer(document.at("version")) != 1) {
        throw storage_failure{storage_error::unsupported_version};
    }
    auto& array = document.at(key);
    if (!array.is_array()) {
        invalid();
    }
    if (std::cmp_greater(array.size(), limit)) {
        throw storage_failure{storage_error::resource_limit};
    }
    return array;
}

} // namespace

auto encode_index(std::span<const profile_record> records) -> std::string {
    try {
        auto profiles = json::array();
        for (const auto& record : records) {
            profiles.push_back(encode(record));
        }
        return json{{"version", 1}, {"profiles", std::move(profiles)}}.dump();
    } catch (const codec_failure&) {
        throw storage_failure{storage_error::invalid_input};
    } catch (const json::exception&) {
        throw storage_failure{storage_error::invalid_input};
    }
}

auto decode_index(std::string_view text, std::int64_t max_profiles) -> std::vector<profile_record> {
    // 兼容旧字段名只发生在解码边界。新旧名称同时出现时拒绝，避免同一语义
    // 出现两个相互矛盾的值；重新编码始终写当前名称。
    try {
        auto document = parse_document(text);
        object_size(document, 2);
        // 校验后复用同一数组；字段迁移只改记录内容，不改变信封或数组长度。
        auto& profiles = document_array(document, "profiles", max_profiles);

        for (auto& profile : profiles) {
            auto& settings = profile.at("settings");
            if (!settings.is_object()) {
                invalid();
            }
            const auto legacy = settings.find("keep_original_rules");
            if (legacy != settings.end()) {
                if (settings.contains("preserve_original_rules") || !legacy->is_boolean()) {
                    invalid();
                }
                settings.get_ref<json::object_t&>().emplace("preserve_original_rules", *legacy);
                settings.erase(legacy);
            }
        }
        return decode<std::vector<profile_record>>(profiles);
    } catch (const codec_failure&) {
        throw storage_failure{storage_error::corrupt_index};
    } catch (const json::exception&) {
        throw storage_failure{storage_error::corrupt_index};
    }
}

auto encode_nodes(std::span<const node_model::unified_node> nodes) -> std::string {
    try {
        auto output = std::string{R"({"version":1,"model_schema":)"};
        write_cache_literal(output, model_schema);
        output += ",\"nodes\":[";
        auto comma = false;
        for (const auto& node : nodes) {
            if (comma) {
                output += ',';
            }
            comma = true;
            write_cache_value(output, node);
        }
        output += "]}";
        return output;
    } catch (const codec_failure&) {
        throw storage_failure{storage_error::invalid_input};
    } catch (const json::exception&) {
        throw storage_failure{storage_error::invalid_input};
    }
}

auto decode_nodes(std::string_view text, std::int64_t max_nodes)
    -> std::vector<node_model::unified_node> {
    // 节点缓存用 SAX 直接构造强类型节点，不先建立整份通用 JSON 树。
    // reader 同时检查 schema、字段、重复键和节点上限；持久化缓存仍按不可信输入处理。
    try {
        auto reader = node_cache_reader{max_nodes};
        if (!json::sax_parse(text, &reader)) {
            invalid();
        }
        return reader.take_nodes();
    } catch (const codec_failure&) {
        throw storage_failure{storage_error::corrupt_cache};
    } catch (const json::exception&) {
        throw storage_failure{storage_error::corrupt_cache};
    }
}
} // namespace profile_storage::detail

// NOLINTEND(llvm-prefer-static-over-anonymous-namespace,altera-struct-pack-align,readability-redundant-member-init,altera-unroll-loops,fuchsia-default-arguments-calls)
