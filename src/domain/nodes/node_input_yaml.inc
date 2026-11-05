

// NOLINTBEGIN(llvm-prefer-static-over-anonymous-namespace)
// NOLINTBEGIN(fuchsia-default-arguments-calls)
// NOLINTBEGIN(fuchsia-default-arguments-declarations)
// NOLINTBEGIN(altera-unroll-loops)
// NOLINTBEGIN(altera-struct-pack-align)
// NOLINTBEGIN(abseil-string-find-str-contains)
// NOLINTBEGIN(readability-redundant-member-init)

[[nodiscard]] auto yaml_view(ryml::csubstr text) noexcept -> std::string_view {
    return text.len == 0 ? std::string_view{} : std::string_view{text.str, text.len};
}

[[noreturn]] auto yaml_basic_error(ryml::csubstr message, const ryml::ErrorDataBasic& location,
                                   void* /*context*/) -> void {
    static_cast<void>(message);
    static_cast<void>(location);
    fail(parse_error::invalid_document);
}

[[noreturn]] auto yaml_parse_error(ryml::csubstr message, const ryml::ErrorDataParse& location,
                                   void* /*context*/) -> void {
    static_cast<void>(message);
    static_cast<void>(location);
    fail(parse_error::invalid_document);
}

[[noreturn]] auto yaml_visit_error(ryml::csubstr message, const ryml::ErrorDataVisit& location,
                                   void* /*context*/) -> void {
    static_cast<void>(message);
    static_cast<void>(location);
    fail(parse_error::invalid_document);
}

// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
// YAML 解析树的节点数量与别名展开后的值数量分别受限。path 跟踪当前展开链，
// 用于发现循环引用；anchors 保存同名锚点的位置，引用只能选择此前的定义。
// 这些引用只在本次解析栈内借用，最终节点不依赖 ryml::Tree 的生命期。
struct yaml_context {
    yaml_context(const ryml::Tree& input_tree, const parse_limits& input_limits)
        : tree{input_tree}, limits{input_limits} {}

    std::reference_wrapper<const ryml::Tree> tree;
    std::reference_wrapper<const parse_limits> limits;
    std::map<std::string, std::vector<ryml::id_type>, std::less<>> anchors{};
    std::vector<ryml::id_type> path{};
    std::int64_t expanded{};
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

[[nodiscard]] auto yaml_reference(const yaml_context& context, ryml::ConstNodeRef node)
    -> ryml::ConstNodeRef {
    const auto found = context.anchors.find(yaml_view(node.val_ref()));
    if (found == context.anchors.end()) {
        fail(parse_error::invalid_parameter);
    }
    const auto before = std::ranges::lower_bound(found->second, node.id());
    if (before == found->second.begin()) {
        fail(parse_error::invalid_parameter);
    }
    return context.tree.get().cref(*std::prev(before));
}

auto yaml_merge(json& object, const json& inherited) -> void {
    if (inherited.is_array()) {
        for (const auto& item : inherited) {
            if (!item.is_object()) {
                fail(parse_error::invalid_parameter);
            }
            for (const auto& [key, value] : item.items()) {
                if (!object.contains(key)) {
                    set_json_member(object, key, value);
                }
            }
        }
    } else if (inherited.is_object()) {
        for (const auto& [key, value] : inherited.items()) {
            if (!object.contains(key)) {
                set_json_member(object, key, value);
            }
        }
    } else {
        fail(parse_error::invalid_parameter);
    }
}

[[nodiscard]] auto yaml_value(yaml_context& context, ryml::ConstNodeRef node) -> json;

// NOLINTNEXTLINE(misc-no-recursion)
[[nodiscard]] auto yaml_mapping(yaml_context& context, ryml::ConstNodeRef node) -> json {
    auto result = json::object();
    auto explicit_keys = std::unordered_set<std::string>{};
    for (const auto child : node.children()) {
        if (!child.has_key() || child.is_key_ref()) {
            fail(parse_error::invalid_parameter);
        }
        const auto key = std::string{yaml_view(child.key())};
        if (key == "<<" && !child.is_key_quoted()) {
            yaml_merge(result, yaml_value(context, child));
        }
    }
    for (const auto child : node.children()) {
        const auto key = std::string{yaml_view(child.key())};
        if (key == "<<" && !child.is_key_quoted()) {
            continue;
        }
        if (explicit_keys.contains(key)) {
            fail(parse_error::invalid_parameter);
        }
        if (std::ssize(explicit_keys) >= context.limits.get().max_parameters) {
            fail(parse_error::resource_limit);
        }
        explicit_keys.insert(key);
        set_json_member(result, key, yaml_value(context, child));
    }
    return result;
}

// NOLINTNEXTLINE(misc-no-recursion)
[[nodiscard]] auto yaml_value(yaml_context& context, ryml::ConstNodeRef node) -> json {
    if (++context.expanded > context.limits.get().max_expanded_values ||
        std::ssize(context.path) >= context.limits.get().max_json_depth) {
        fail(parse_error::resource_limit);
    }
    if (std::ranges::find(context.path, node.id()) != context.path.end()) {
        fail(parse_error::invalid_parameter);
    }
    context.path.push_back(node.id());
    auto result = json{};
    if (node.is_val_ref()) {
        result = yaml_value(context, yaml_reference(context, node));
    } else if (node.is_map()) {
        result = yaml_mapping(context, node);
    } else if (node.is_seq()) {
        result = json::array();
        for (const auto child : node.children()) {
            result.push_back(yaml_value(context, child));
        }
    } else if (node.has_val() && !node.val_is_null()) {

        result = std::string{yaml_view(node.val())};
    }
    context.path.pop_back();
    return result;
}

[[nodiscard]] auto yaml_newlines(std::string_view input) -> std::string {
    auto result = std::string{};
    result.reserve(input.size());
    auto preceding_cr = false;
    for (const auto character : input) {
        if (character == '\n' && preceding_cr) {
            preceding_cr = false;
            continue;
        }
        result += character == '\r' ? '\n' : character;
        preceding_cr = character == '\r';
    }
    return result;
}

template <bool track_locations, typename Visitor>
    requires std::invocable<Visitor, yaml_context&, ryml::ConstNodeRef, ryml::Parser&>
auto visit_yaml_root(std::string_view input, const parse_limits& limits, Visitor&& visitor)
    -> void {
    const auto normalized =
        input.find('\r') == std::string_view::npos ? std::string{} : yaml_newlines(input);
    if (!normalized.empty()) {
        input = normalized;
    }
    auto callbacks = ryml::Callbacks{};
    callbacks.set_error_basic(yaml_basic_error);
    callbacks.set_error_parse(yaml_parse_error);
    callbacks.set_error_visit(yaml_visit_error);
    auto tree = ryml::Tree{callbacks};
    auto handler = ryml::EventHandlerTree{callbacks};
    auto parser_options = ryml::ParserOptions{};
    parser_options.locations(track_locations);
    auto parser = ryml::Parser{&handler, parser_options};
    ryml::parse_in_arena(&parser, ryml::csubstr{input.data(), input.size()}, &tree);
    if (tree.size() == 0 || tree.size() > static_cast<ryml::id_type>(limits.max_yaml_nodes)) {
        fail(parse_error::resource_limit);
    }
    auto root = tree.crootref();
    if (root.is_stream()) {
        if (root.num_children() != 1) {
            fail(parse_error::invalid_document);
        }
        root = *root.children().begin();
    }
    if (!root.is_map()) {
        fail(parse_error::invalid_document);
    }
    auto context = yaml_context{tree, limits};
    for (auto index = std::int64_t{}; std::cmp_less(index, tree.size()); ++index) {
        const auto node = tree.cref(static_cast<ryml::id_type>(index));
        if (node.has_val_anchor()) {
            context.anchors[std::string{yaml_view(node.val_anchor())}].push_back(node.id());
        }
    }
    std::invoke(std::forward<Visitor>(visitor), context, root, parser);
}

auto parse_clash_yaml(std::string_view input, const parse_limits& limits,
                      subscription_result& result) -> void {
    visit_yaml_root<true>(
        input, limits,
        [&](yaml_context& context, ryml::ConstNodeRef root, ryml::Parser& parser) -> void {
            auto proxies = root.find_child("proxies");
            if (!proxies.readable()) {
                fail(parse_error::invalid_document);
            }
            for (const auto child : root.children()) {
                if (child.has_key() && yaml_view(child.key()) == "proxies" &&
                    child.id() != proxies.id()) {
                    fail(parse_error::invalid_document);
                }
            }
            if (proxies.is_val_ref()) {
                proxies = yaml_reference(context, proxies);
            }
            if (!proxies.is_seq()) {
                fail(parse_error::invalid_document);
            }
            auto index = std::int64_t{};
            for (const auto node : proxies.children()) {
                if (++index > limits.max_nodes || context.expanded >= limits.max_expanded_values) {
                    fail(parse_error::resource_limit);
                }
                const auto line =
                    static_cast<std::int64_t>(context.tree.get().location(parser, node.id()).line) +
                    1;
                append_document_node(
                    result, {.line = line, .index = index, .collection = node_collection::proxies},
                    [&]() -> node_parse_result {
                        context.path.clear();
                        return clash_node(yaml_value(context, node), limits);
                    });
            }
        });
}

[[nodiscard]] auto material_yaml_hint(std::string_view input, const parse_limits& limits) -> bool {
    return input.starts_with('{') || has_yaml_root_key(input, limits) ||
           std::ranges::any_of(
               text_lines(input, limits), [](std::string_view line) noexcept -> bool {
                   if (line.empty() || ascii_space(line.front()) || line.front() == '#') {
                       return false;
                   }
                   const auto key = trim(line.substr(0, line.find(':')));
                   return key == "proxy-groups" || key == "rules" || key == "rule-providers" ||
                          key == "\"proxy-groups\"" || key == "'proxy-groups'" ||
                          key == "\"rules\"" || key == "'rules'" || key == "\"rule-providers\"" ||
                          key == "'rule-providers'";
               });
}

[[nodiscard]] auto material_sections(std::string_view input, const parse_limits& limits)
    -> material::clash_material {
    auto result = material::clash_material{.sections = json::object()};
    visit_yaml_root<false>(
        input, limits, [&](yaml_context& context, ryml::ConstNodeRef root, ryml::Parser&) -> void {
            for (const auto child : root.children()) {
                if (!child.has_key() || child.is_key_ref()) {
                    fail(parse_error::invalid_document);
                }
                const auto key = yaml_view(child.key());
                if (key == "<<" && !child.is_key_quoted()) {
                    fail(parse_error::unsupported_parameter);
                }
                if (key != "proxy-groups" && key != "rules" && key != "rule-providers") {
                    continue;
                }
                if (result.sections.contains(key)) {
                    fail(parse_error::invalid_document);
                }
                result.found = true;
                set_json_member(result.sections, std::string{key}, yaml_value(context, child));
            }
        });
    return result;
}

[[nodiscard]] auto read_clash_material(std::string_view input, const parse_limits& limits)
    -> material::clash_material {
    auto decoded = std::string{};
    constexpr int maximum_unwraps{2};
    for (auto unwraps = int{}; unwraps <= maximum_unwraps; ++unwraps) {
        input = remove_bom(input);
        if (input.empty() || starts_with_uri(input) || has_wireguard_section(input, limits) ||
            input.starts_with('[')) {
            return {};
        }
        if (material_yaml_hint(input, limits)) {
            return material_sections(input, limits);
        }
        if (has_scheme(input) || input.starts_with('#') || input.starts_with("//")) {
            return {};
        }
        if (unwraps == maximum_unwraps) {
            fail(parse_error::invalid_document);
        }
        auto next = decode_base64(input, true);
        decoded.swap(next);
        input = decoded;
    }
    return {};
}

// NOLINTEND(readability-redundant-member-init)
// NOLINTEND(abseil-string-find-str-contains)
// NOLINTEND(altera-struct-pack-align)
// NOLINTEND(altera-unroll-loops)
// NOLINTEND(fuchsia-default-arguments-declarations)
// NOLINTEND(fuchsia-default-arguments-calls)
// NOLINTEND(llvm-prefer-static-over-anonymous-namespace)
