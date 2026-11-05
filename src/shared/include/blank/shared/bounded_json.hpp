#ifndef BLANK_QUICK_BOUNDED_JSON_HPP // NOLINT(llvm-header-guard)
#define BLANK_QUICK_BOUNDED_JSON_HPP

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace json_detail {

// NOLINTBEGIN(altera-struct-pack-align,altera-unroll-loops,fuchsia-default-arguments-calls,readability-redundant-member-init)
enum class read_error : std::int8_t { none, invalid_document, resource_limit };
struct read_options {
    std::int64_t max_event_depth{};
    std::int64_t max_values{std::numeric_limits<std::int64_t>::max()};
    bool reject_duplicates{};
    bool ignore_comments{};
};

template <typename Json>
concept json_document =
    std::same_as<Json, nlohmann::json> || std::same_as<Json, nlohmann::ordered_json>;

template <json_document Json> struct read_result {
    Json value{};
    read_error error{};
};

// 在 SAX 事件到来时限制深度/值数量，不等整棵树分配完才检查。
// 对象栈各自记录键索引，因此可拒绝重复键；ordered_json 同时保留输入顺序。
// 此工具只负责结构边界，字段类型、版本和业务引用仍由调用模块验证。
template <json_document Json> class document_reader final : public nlohmann::json_sax<Json> {
  public:
    explicit document_reader(const read_options& selected) noexcept : options{selected} {}
    auto null() -> bool override {
        return scalar(nullptr);
    }
    auto boolean(bool value) -> bool override {
        return scalar(value);
    }
    auto number_integer(Json::number_integer_t value) -> bool override {
        return scalar(value);
    }
    auto number_unsigned(Json::number_unsigned_t value) -> bool override {
        return scalar(value);
    }
    auto number_float(Json::number_float_t value, [[maybe_unused]] const Json::string_t& token)
        -> bool override {
        return scalar(value);
    }
    auto string(Json::string_t& value) -> bool override {
        return scalar(std::move(value));
    }
    auto binary([[maybe_unused]] Json::binary_t& value) noexcept -> bool override {
        return reject(read_error::invalid_document);
    }
    auto start_object([[maybe_unused]] std::size_t elements) -> bool override {
        return start(Json::object());
    }
    auto start_array([[maybe_unused]] std::size_t elements) -> bool override {
        return start(Json::array());
    }
    auto end_object() -> bool override {
        return finish();
    }
    auto end_array() -> bool override {
        return finish();
    }
    auto key(Json::string_t& value) -> bool override {
        if (!depth_allowed()) {
            return false;
        }
        auto& current = frames.back();
        if (options.reject_duplicates || std::same_as<Json, nlohmann::ordered_json>) {
            const auto [found, inserted] =
                current.indices.try_emplace(value, current.indices.size());
            if (!inserted && options.reject_duplicates) {
                return reject(read_error::invalid_document);
            }
            current.index = found->second;
        }
        current.key = std::move(value);
        return true;
    }
    auto parse_error([[maybe_unused]] std::size_t position,
                     [[maybe_unused]] const std::string& token,
                     [[maybe_unused]] const Json::exception& failure) noexcept -> bool override {
        return reject(read_error::invalid_document);
    }
    [[nodiscard]] auto take(bool accepted) noexcept -> read_result<Json> {
        if (!accepted) {
            return {.error = error == read_error::none ? read_error::invalid_document : error};
        }
        return {.value = std::move(root)};
    }

  private:
    struct frame { // NOLINT(bugprone-exception-escape)
        Json value{};
        std::unordered_map<std::string, std::size_t> indices{};
        std::string key{};
        std::size_t index{};
    };
    read_options options{};
    Json root{};
    std::vector<frame> frames{};
    std::int64_t values{};
    read_error error{};

    auto reject(read_error selected) noexcept -> bool {
        error = selected;
        return false;
    }
    auto depth_allowed() noexcept -> bool {
        return std::cmp_greater(frames.size(), options.max_event_depth)
                   ? reject(read_error::resource_limit)
                   : true;
    }
    auto accept_value() noexcept -> bool {
        if (!depth_allowed()) {
            return false;
        }
        return ++values > options.max_values ? reject(read_error::resource_limit) : true;
    }
    auto append(Json value) -> void {
        if (frames.empty()) {
            root = std::move(value);
            return;
        }
        auto& current = frames.back();
        if (current.value.is_array()) {
            current.value.push_back(std::move(value));
        } else if constexpr (std::same_as<Json, nlohmann::ordered_json>) {
            auto& object = current.value.template get_ref<typename Json::object_t&>();
            if (current.index == object.size()) {

                object.emplace_back(std::move(current.key), std::move(value));
            } else {
                static_cast<Json::object_t::Container&>(object).at(current.index).second =
                    std::move(value);
            }
        } else {
            current.value.template get_ref<typename Json::object_t&>().insert_or_assign(
                std::move(current.key), std::move(value));
        }
    }
    auto scalar(Json value) -> bool {
        if (!accept_value()) {
            return false;
        }
        append(std::move(value));
        return true;
    }
    auto start(Json value) -> bool {
        if (!accept_value()) {
            return false;
        }
        frames.push_back({.value = std::move(value)});
        return true;
    }
    auto finish() -> bool {
        auto value = std::move(frames.back().value);
        frames.pop_back();
        append(std::move(value));
        return true;
    }
};

template <json_document Json>
[[nodiscard]] auto read(std::string_view input, const read_options& options) -> read_result<Json> {
    auto reader = document_reader<Json>{options};
    const auto accepted =
        Json::sax_parse(input, &reader, Json::input_format_t::json, true, options.ignore_comments);
    return reader.take(accepted);
}
// NOLINTEND(altera-struct-pack-align,altera-unroll-loops,fuchsia-default-arguments-calls,readability-redundant-member-init)
} // namespace json_detail

#endif
