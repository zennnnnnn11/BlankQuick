// Private document adapters share the existing typed Mihomo binding machinery.
// ISO C++20 CPU adapters use the same local style exceptions as node_input.cpp.
// NOLINTBEGIN(llvm-prefer-static-over-anonymous-namespace)
// NOLINTBEGIN(fuchsia-default-arguments-calls)
// NOLINTBEGIN(fuchsia-default-arguments-declarations)
// NOLINTBEGIN(altera-unroll-loops)
// NOLINTBEGIN(altera-struct-pack-align)
// NOLINTBEGIN(abseil-string-find-str-contains)
// NOLINTBEGIN(readability-redundant-member-init)

auto set_json_member(json& object, std::string key, json value) -> void {
    object.get_ref<json::object_t&>().insert_or_assign(std::move(key), std::move(value));
}

struct document_location {
    std::int64_t line{};
    std::int64_t index{};
    node_collection collection{node_collection::nodes};
};

[[nodiscard]] auto peek_option(const options& source, std::string_view key) -> const json* {
    const auto found = source.values.find(normalize_key(key));
    return found == source.values.end() ? nullptr : &found->second.value;
}

[[nodiscard]] auto skip_json_preamble(std::string_view input) noexcept -> std::string_view {
    auto candidate = input;
    while (true) {
        candidate = trim(candidate);
        if (candidate.starts_with("//")) {
            const auto end = candidate.find_first_of("\r\n");
            if (end == std::string_view::npos) {
                return input;
            }
            candidate.remove_prefix(end);
        } else if (candidate.starts_with("/*")) {
            const auto end = candidate.find("*/", 2);
            if (end == std::string_view::npos) {
                return input;
            }
            candidate.remove_prefix(end + 2);
        } else {
            break;
        }
    }
    return candidate.starts_with('{') || candidate.starts_with('[') ? candidate : input;
}

auto record_node_error(subscription_result& result, const document_location& location,
                       parse_error error) -> void {
    result.diagnostics.push_back({
        .line = location.line,
        .error = error,
        .index = location.index,
        .collection = location.collection,
    });
}

template <typename factory_type>
    requires std::invocable<factory_type> &&
             std::same_as<std::invoke_result_t<factory_type>, node_parse_result>
auto append_document_node(subscription_result& result, const document_location& location,
                          factory_type&& factory) -> void {
    try {
        auto parsed = std::invoke(std::forward<factory_type>(factory));
        if (parsed.error != parse_error::none || !parsed.ignored_parameters.empty()) {
            result.diagnostics.push_back({
                .line = location.line,
                .error = parsed.error,
                .ignored_parameters = std::move(parsed.ignored_parameters),
                .index = location.index,
                .collection = location.collection,
            });
        }
        if (parsed.node.has_value()) {
            result.nodes.push_back(std::move(*parsed.node));
        }
    } catch (const input_failure& failure) {
        record_node_error(result, location, failure.error);
    } catch (const json::exception&) {
        record_node_error(result, location, parse_error::invalid_parameter);
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception&) {
        record_node_error(result, location, parse_error::unexpected_failure);
    }
}

[[nodiscard]] auto object_options(const json& object, const parse_limits& limits) -> options {
    if (!object.is_object()) {
        fail(parse_error::invalid_parameter);
    }
    auto result = options{.limits = limits};
    for (const auto& [key, value] : object.items()) {
        if (value.is_null()) {
            continue;
        }
        // Case/dash normalization must not make two different keys silently collide.
        if (result.values.contains(normalize_key(key))) {
            fail(parse_error::invalid_parameter);
        }
        set_option(result, key, value);
    }
    return result;
}

[[nodiscard]] auto raw_endpoint(options& source) -> node_model::remote_endpoint {
    auto host = option_text(source, "server");
    if (host.find('%') != std::string::npos) {
        fail(parse_error::invalid_endpoint);
    }
    if (host.starts_with('[') && host.ends_with(']')) {
        host = host.substr(1, host.size() - 2);
    }
    const auto port = option_text(source, "port");
    return endpoint(
        host.find(':') == std::string::npos ? host + ':' + port : '[' + host + "]:" + port, 0);
}

[[nodiscard]] auto raw_parts(options source, uri_protocol protocol, std::string name) -> uri_parts {
    validate_text(name);
    auto result =
        uri_parts{.protocol = protocol, .fragment = std::move(name), .fields = std::move(source)};
    if (protocol == uri_protocol::wireguard && !result.fields.values.contains("server")) {
        const auto* peers = find_option(result.fields, "peers");
        if (peers == nullptr || !peers->is_array() || peers->empty()) {
            fail(parse_error::missing_credential);
        }
        auto peer = object_options(peers->front(), result.fields.limits);
        result.remote = raw_endpoint(peer);
    } else {
        result.remote = raw_endpoint(result.fields);
    }
    // Mihomo network:http is HTTP camouflage; share-link type:http means H2.
    if (lower(option_text(result.fields, "network")) == "http") {
        set_option(result.fields, "network", "tcp");
        set_option(result.fields, "headerType", "http");
    }
    return result;
}

[[nodiscard]] auto clash_node(const json& object, const parse_limits& limits) -> node_parse_result {
    auto source = object_options(object, limits);
    const auto protocol = parse_protocol(option_text(source, "type"));
    source.values.erase("type");
    auto name = option_text(source, "name");
    source.values.erase("name");
    return assemble(raw_parts(std::move(source), protocol, std::move(name)), false);
}

auto extract_clash_json(const json& proxies, const parse_limits& limits,
                        subscription_result& result) -> void {
    if (!proxies.is_array()) {
        fail(parse_error::invalid_document);
    }
    auto index = std::int64_t{};
    for (const auto& object : proxies) {
        if (++index > limits.max_nodes) {
            fail(parse_error::resource_limit);
        }
        append_document_node(result, {.index = index, .collection = node_collection::proxies},
                             [&]() -> node_parse_result { return clash_node(object, limits); });
    }
}

[[nodiscard]] constexpr auto starts_with_uri(std::string_view input) noexcept -> bool {
    const auto end = input.find_first_of("\r\n");
    const auto scheme = input.find("://");
    if (scheme == std::string_view::npos || (end != std::string_view::npos && scheme > end)) {
        return false;
    }
    const auto prefix = input.substr(0, scheme);
    return !prefix.empty() && std::ranges::all_of(prefix, [](char character) noexcept -> bool {
        return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') || character == '+' || character == '-';
    });
}

[[nodiscard]] auto text_lines(std::string_view text, const parse_limits& limits)
    -> std::vector<std::string_view> {
    auto result = std::vector<std::string_view>{};
    while (!text.empty()) {
        if (std::ssize(result) >= limits.max_lines) {
            fail(parse_error::resource_limit);
        }
        const auto end = text.find_first_of("\r\n");
        result.push_back(text.substr(0, end));
        if (end == std::string_view::npos) {
            break;
        }
        const auto crlf = text.at(end) == '\r' && end + 1 < text.size() && text.at(end + 1) == '\n';
        text.remove_prefix(end + (crlf ? 2 : 1));
    }
    return result;
}

[[nodiscard]] auto has_wireguard_section(std::string_view input, const parse_limits& limits)
    -> bool {
    return std::ranges::any_of(text_lines(input, limits), [](std::string_view line) -> bool {
        return lower(trim(line.substr(0, line.find_first_of("#;")))) == "[interface]";
    });
}

[[nodiscard]] auto has_yaml_root_key(std::string_view input, const parse_limits& limits) -> bool {
    return input.starts_with("---") ||
           std::ranges::any_of(
               text_lines(input, limits), [](std::string_view line) noexcept -> bool {
                   if (line.empty() || ascii_space(line.front()) || line.front() == '#') {
                       return false;
                   }
                   const auto key = trim(line.substr(0, line.find(':')));
                   return key == "proxies" || key == "\"proxies\"" || key == "'proxies'";
               });
}

auto parse_clash_yaml(std::string_view input, const parse_limits& limits,
                      subscription_result& result) -> void;

// NOLINTEND(readability-redundant-member-init)
// NOLINTEND(abseil-string-find-str-contains)
// NOLINTEND(altera-struct-pack-align)
// NOLINTEND(altera-unroll-loops)
// NOLINTEND(fuchsia-default-arguments-declarations)
// NOLINTEND(fuchsia-default-arguments-calls)
// NOLINTEND(llvm-prefer-static-over-anonymous-namespace)
