#include <QtCore/QByteArrayView>

#include <algorithm>
#include <blank/overrides/override_engine.hpp>
#include <blank/overrides/override_types.hpp>
#include <blank/shared/bounded_json.hpp>
#include <blank/shared/ordered_object.hpp>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <rapidyaml/ryml.hpp>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "override_detail.hpp"

namespace overrides::detail {
namespace {

// NOLINTBEGIN(llvm-prefer-static-over-anonymous-namespace,altera-struct-pack-align,readability-redundant-member-init,altera-unroll-loops,fuchsia-default-arguments-calls)

[[nodiscard]] auto view(ryml::csubstr value) noexcept -> std::string_view {
    return value.empty() ? std::string_view{} : std::string_view{value.str, value.len};
}

[[noreturn]] auto basic_error(ryml::csubstr, const ryml::ErrorDataBasic&, void*) -> void {
    fail(override_error::invalid_document);
}
[[noreturn]] auto parse_error(ryml::csubstr, const ryml::ErrorDataParse&, void*) -> void {
    fail(override_error::invalid_document);
}
[[noreturn]] auto visit_error(ryml::csubstr, const ryml::ErrorDataVisit&, void*) -> void {
    fail(override_error::invalid_document);
}

// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
struct yaml_state {
    yaml_state(const ryml::Tree& input_tree, const engine_options& input_options)
        : tree{input_tree}, options{input_options} {}
    std::reference_wrapper<const ryml::Tree> tree;
    std::reference_wrapper<const engine_options> options;
    std::map<std::string, std::vector<ryml::id_type>, std::less<>> anchors{};
    std::vector<ryml::id_type> path{};
    std::int64_t expanded{};
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

[[nodiscard]] constexpr auto tag_is(std::string_view tag, std::string_view type) -> bool {
    return tag == std::string{"!!"} + std::string{type} ||
           tag == std::string{"tag:yaml.org,2002:"} + std::string{type} ||
           tag == std::string{"!<tag:yaml.org,2002:"} + std::string{type} + ">";
}

[[nodiscard]] auto implicit_scalar(ryml::ConstNodeRef node) -> document {
    const auto text = view(node.val());
    if (node.val_is_null() || text == "null" || text == "Null" || text == "NULL" || text == "~") {
        return nullptr;
    }
    if (text == "true" || text == "True" || text == "TRUE") {
        return true;
    }
    if (text == "false" || text == "False" || text == "FALSE") {
        return false;
    }
    auto result = document::parse(text, nullptr, false);
    if (result.is_discarded() || !result.is_number()) {
        return std::string{text};
    }
    return result;
}

[[nodiscard]] auto scalar(ryml::ConstNodeRef node) -> document {
    const auto tag = node.has_val_tag() ? view(node.val_tag()) : std::string_view{};
    if (tag_is(tag, "str") || (tag.empty() && node.is_val_quoted())) {
        return std::string{view(node.val())};
    }
    auto result = implicit_scalar(node);
    if (!tag.empty() && !((tag_is(tag, "null") && result.is_null()) ||
                          (tag_is(tag, "bool") && result.is_boolean()) ||
                          (tag_is(tag, "int") && result.is_number_integer()) ||
                          (tag_is(tag, "float") && result.is_number()))) {
        fail(override_error::invalid_document);
    }
    return result;
}

[[nodiscard]] auto yaml_value(yaml_state& state, ryml::ConstNodeRef node) -> document;

auto inherit_mapping(json_detail::ordered_object& target, const document& inherited) -> void {
    if (!inherited.is_object()) {
        fail(override_error::invalid_document);
    }
    for (const auto& [key, value] : inherited.items()) {
        if (!target.contains(key)) {
            target.member(key) = value;
        }
    }
}

// NOLINTNEXTLINE(misc-no-recursion)
[[nodiscard]] auto yaml_mapping(yaml_state& state, ryml::ConstNodeRef node) -> document {
    auto result = document::object();
    auto indexed = json_detail::ordered_object{result};
    auto keys = std::unordered_set<std::string>{};
    for (const auto child : node.children()) {
        if (!child.has_key() || child.is_key_ref() || child.has_key_tag()) {
            fail(override_error::invalid_document);
        }
        const auto key = std::string{view(child.key())};
        if (!keys.insert(key).second) {
            fail(override_error::invalid_document);
        }
        if (key == "<<" && !child.is_key_quoted()) {
            const auto inherited = yaml_value(state, child);
            if (inherited.is_array()) {
                for (const auto& value : inherited) {
                    inherit_mapping(indexed, value);
                }
            } else {
                inherit_mapping(indexed, inherited);
            }
        }
    }
    for (const auto child : node.children()) {
        const auto key = std::string{view(child.key())};
        if (key != "<<" || child.is_key_quoted()) {

            indexed.member(key) = yaml_value(state, child);
        }
    }
    return result;
}

[[nodiscard]] auto referenced(const yaml_state& state, ryml::ConstNodeRef node)
    -> ryml::ConstNodeRef {
    const auto found = state.anchors.find(view(node.val_ref()));
    if (found == state.anchors.end()) {
        fail(override_error::invalid_document);
    }
    const auto before = std::ranges::lower_bound(found->second, node.id());
    if (before == found->second.begin()) {
        fail(override_error::invalid_document);
    }
    return state.tree.get().cref(*std::prev(before));
}

// NOLINTNEXTLINE(misc-no-recursion)
[[nodiscard]] auto yaml_value(yaml_state& state, ryml::ConstNodeRef node) -> document {
    if (++state.expanded > state.options.get().max_values ||
        std::ssize(state.path) >= state.options.get().max_depth) {
        fail(override_error::resource_limit);
    }
    if (std::ranges::find(state.path, node.id()) != state.path.end()) {
        fail(override_error::invalid_document);
    }
    state.path.push_back(node.id());
    auto result = document{};
    const auto tag = node.has_val_tag() ? view(node.val_tag()) : std::string_view{};
    if (node.is_val_ref()) {
        result = yaml_value(state, referenced(state, node));
    } else if (node.is_map()) {
        if (!tag.empty() && !tag_is(tag, "map")) {
            fail(override_error::invalid_document);
        }
        result = yaml_mapping(state, node);
    } else if (node.is_seq()) {
        if (!tag.empty() && !tag_is(tag, "seq")) {
            fail(override_error::invalid_document);
        }
        result = document::array();
        for (const auto child : node.children()) {
            result.push_back(yaml_value(state, child));
        }
    } else if (node.has_val()) {
        result = scalar(node);
    }
    state.path.pop_back();
    return result;
}

// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
struct value_location {
    value_location(const document& input_value, std::int64_t input_depth) noexcept
        : value{input_value}, depth{input_depth} {}
    std::reference_wrapper<const document> value;
    std::int64_t depth{};
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

enum class merge_verb : std::int8_t { merge, prepend, append, replace };
struct merge_key {
    std::string_view name{};
    merge_verb verb{merge_verb::merge};
};

[[nodiscard]] auto read_key(std::string_view key) -> merge_key {
    auto result = merge_key{.name = key};
    if (key.starts_with('+')) {
        result = {.name = key.substr(1), .verb = merge_verb::prepend};
    } else if (key.ends_with('+')) {
        result = {.name = key.substr(0, key.size() - 1), .verb = merge_verb::append};
    } else if (key.ends_with('!')) {
        result = {.name = key.substr(0, key.size() - 1), .verb = merge_verb::replace};
    }
    if (result.name.empty() || result.name.starts_with('+') || result.name.ends_with('+') ||
        result.name.ends_with('!')) {
        fail(override_error::invalid_operation);
    }
    return result;
}

auto insert_sequence(document& target, const document& patch, merge_verb verb) -> void {
    if (!patch.is_array() || (!target.is_null() && !target.is_array())) {
        fail(override_error::invalid_operation);
    }
    if (target.is_null()) {
        target = document::array();
    }
    const auto position = verb == merge_verb::prepend ? target.begin() : target.end();
    target.insert(position, patch.begin(), patch.end());
}

// NOLINTNEXTLINE(misc-no-recursion)
auto merge_mapping(document& target, const document& patch, std::int64_t depth,
                   const engine_options& options) -> void {
    if (depth >= options.max_depth) {
        fail(override_error::resource_limit);
    }
    auto indexed = json_detail::ordered_object{target};
    for (const auto& [raw_key, value] : patch.items()) {
        const auto key = read_key(raw_key);
        auto& destination = indexed.member(key.name);
        if (key.verb == merge_verb::prepend || key.verb == merge_verb::append) {
            insert_sequence(destination, value, key.verb);
        } else if (key.verb == merge_verb::merge && value.is_object()) {
            if (!destination.is_object()) {
                destination = document::object();
            }
            merge_mapping(destination, value, depth + 1, options);
        } else {
            destination = value;
        }
    }
}
} // namespace

auto valid_identifier(std::string_view value) noexcept -> bool {
    constexpr std::int64_t identifier_size{32};
    return std::ssize(value) == identifier_size &&
           std::ranges::all_of(value, [](char character) noexcept -> bool {
               return (character >= '0' && character <= '9') ||
                      (character >= 'a' && character <= 'f');
           });
}

auto valid_utf8(std::string_view value) noexcept -> bool {
    return QByteArrayView{value}.isValidUtf8();
}

auto valid_options(const engine_options& options) noexcept -> bool {
    constexpr auto maximum_depth = 128;
    constexpr auto maximum_limit = std::numeric_limits<int>::max();
    return options.max_document_bytes > 0 && options.max_document_bytes < maximum_limit &&
           options.max_patch_bytes > 0 && options.max_patch_bytes < maximum_limit &&
           options.max_values > 0 && options.max_values < maximum_limit && options.max_depth > 0 &&
           options.max_depth <= maximum_depth && options.max_patches > 0 &&
           options.max_patches < maximum_limit && options.max_log_lines >= 0 &&
           options.max_log_lines < maximum_limit && options.max_log_bytes >= 0 &&
           options.max_log_bytes < maximum_limit && options.script_timeout.count() > 0 &&
           options.script_timeout <= std::chrono::minutes{1} &&
           (options.input_format == configuration_format::json ||
            options.input_format == configuration_format::yaml);
}

auto validate_document(const document& value, const engine_options& options) -> void {
    if (!value.is_object()) {
        fail(override_error::invalid_document);
    }
    auto pending = std::vector<value_location>{value_location{value, 0}};
    auto visited = std::int64_t{};
    while (!pending.empty()) {
        const auto current = pending.back();
        pending.pop_back();
        if (++visited > options.max_values || current.depth >= options.max_depth) {
            fail(override_error::resource_limit);
        }
        const auto& item = current.value.get();
        if (item.is_number_float() && !std::isfinite(item.get<double>())) {
            fail(override_error::invalid_document);
        }
        if (item.is_structured()) {
            if (std::cmp_greater(item.size(), options.max_values - visited)) {
                fail(override_error::resource_limit);
            }
            std::ranges::transform(
                item, std::back_inserter(pending),
                [depth = current.depth](const auto& child) noexcept -> value_location {
                    return {child, depth + 1};
                });
        }
    }
    if (std::ssize(value.dump()) > options.max_document_bytes) {
        fail(override_error::resource_limit);
    }
}

auto parse_json(std::string_view text, const engine_options& options) -> document {
    if (std::ssize(text) > options.max_document_bytes) {
        fail(override_error::resource_limit);
    }
    try {
        auto parsed =
            json_detail::read<document>(text, {
                                                  .max_event_depth = options.max_depth - 1,
                                                  .max_values = options.max_values,
                                                  .reject_duplicates = true,
                                              });
        if (parsed.error == json_detail::read_error::resource_limit) {
            fail(override_error::resource_limit);
        }
        if (parsed.error != json_detail::read_error::none) {
            fail(override_error::invalid_document);
        }
        validate_document(parsed.value, options);
        return std::move(parsed.value);
    } catch (const document::exception&) {
        fail(override_error::invalid_document);
    }
}

auto parse_yaml(std::string_view text, const engine_options& options) -> document {
    if (std::ssize(text) > options.max_document_bytes) {
        fail(override_error::resource_limit);
    }
    if (!valid_utf8(text)) {
        fail(override_error::invalid_document);
    }
    auto callbacks = ryml::Callbacks{};
    callbacks.set_error_basic(basic_error);
    callbacks.set_error_parse(parse_error);
    callbacks.set_error_visit(visit_error);
    auto tree = ryml::Tree{callbacks};
    auto handler = ryml::EventHandlerTree{callbacks};
    auto parser = ryml::Parser{&handler};
    ryml::parse_in_arena(&parser, ryml::csubstr{text.data(), text.size()}, &tree);
    if (std::cmp_greater(tree.size(), options.max_values)) {
        fail(override_error::resource_limit);
    }
    auto root = tree.crootref();
    if (root.is_stream()) {
        if (root.num_children() != 1) {
            fail(override_error::invalid_document);
        }
        root = *root.children().begin();
    }
    auto state = yaml_state{tree, options};
    for (auto index = std::int64_t{}; std::cmp_less(index, tree.size()); ++index) {
        const auto node = tree.cref(static_cast<ryml::id_type>(index));
        if (node.has_val_anchor()) {
            state.anchors[std::string{view(node.val_anchor())}].push_back(node.id());
        }
    }
    auto result = yaml_value(state, root);
    validate_document(result, options);
    return result;
}

auto merge_patch(document& target, const document& patch, const engine_options& options) -> void {
    merge_mapping(target, patch, 0, options);
    validate_document(target, options);
}

} // namespace overrides::detail
// NOLINTEND(llvm-prefer-static-over-anonymous-namespace,altera-struct-pack-align,readability-redundant-member-init,altera-unroll-loops,fuchsia-default-arguments-calls)
