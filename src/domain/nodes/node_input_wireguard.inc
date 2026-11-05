

// NOLINTBEGIN(llvm-prefer-static-over-anonymous-namespace)
// NOLINTBEGIN(fuchsia-default-arguments-calls)
// NOLINTBEGIN(fuchsia-default-arguments-declarations)
// NOLINTBEGIN(altera-unroll-loops)
// NOLINTBEGIN(altera-struct-pack-align)
// NOLINTBEGIN(abseil-string-find-str-contains)
// NOLINTBEGIN(readability-redundant-member-init)

struct wireguard_section {
    json fields = json::object();
    std::int64_t line{};
    bool invalid{};
};

[[nodiscard]] auto conf_key(std::string_view name, bool interface_section) -> std::string {
    const auto key = normalize_key(name);
    const auto peer_only = key == "publickey" || key == "presharedkey" || key == "endpoint" ||
                           key == "allowedips" || key == "persistentkeepalive";
    const auto interface_only =
        key == "privatekey" || key == "address" || key == "dns" || key == "mtu" || key == "fwmark";
    if ((interface_section && peer_only) || (!interface_section && interface_only)) {
        return std::string{interface_section ? "interface." : "peer."} + key;
    }
    constexpr auto fields = std::to_array<std::pair<std::string_view, std::string_view>>({
        {"privatekey", "private-key"},
        {"publickey", "public-key"},
        {"presharedkey", "pre-shared-key"},
        {"address", "address"},
        {"dns", "dns"},
        {"mtu", "mtu"},
        {"reserved", "reserved"},
        {"allowedips", "allowed-ips"},
        {"persistentkeepalive", "persistent-keepalive"},
        {"endpoint", "endpoint"},
        {"fwmark", "routing-mark"},
    });
    const auto found = std::ranges::find(fields, key, &decltype(fields)::value_type::first);
    if (found != fields.end()) {
        return std::string{found->second};
    }
    return std::string{interface_section ? "interface." : "peer."} + key;
}

auto set_conf_field(wireguard_section& section, std::string key, std::string_view value) -> void {
    if (key == "routing-mark" && (value == "off" || value.starts_with("0x"))) {
        if (value == "off") {
            value = "0";
        } else {
            value.remove_prefix(2);
        }
        auto mark = std::int64_t{};
        constexpr int hexadecimal{16};
        const auto parsed = std::from_chars(std::to_address(value.begin()),
                                            std::to_address(value.end()), mark, hexadecimal);
        if (value.empty() || parsed.ec != std::errc{} ||
            parsed.ptr != std::to_address(value.end()) || mark < 0) {
            section.invalid = true;
        }
        set_json_member(section.fields, std::move(key), mark);
    } else if (section.fields.contains(key)) {
        if (key == "address" || key == "allowed-ips" || key == "dns") {
            set_json_member(section.fields, key,
                            section.fields.at(key).get<std::string>() + ',' + std::string{value});
        } else {
            section.invalid = true;
        }
    } else {
        set_json_member(section.fields, std::move(key), value);
    }
}

[[nodiscard]] auto native_wireguard_node(const wireguard_section& interface,
                                         const wireguard_section& peer, const parse_limits& limits)
    -> node_parse_result {
    if (interface.invalid || peer.invalid) {
        fail(parse_error::invalid_parameter);
    }
    auto fields = interface.fields;
    for (const auto& [key, value] : peer.fields.items()) {
        set_json_member(fields, key, value);
    }
    auto source = object_options(fields, limits);
    const auto remote = endpoint(option_text(source, "endpoint"), 0);
    source.values.erase("endpoint");
    set_option(source, "server", remote.host);
    set_option(source, "port", remote.port);
    set_option(source, "udp", true);
    auto result = assemble(raw_parts(std::move(source), uri_protocol::wireguard, {}), false);
    return result;
}

struct wireguard_document {
    wireguard_section interface{};
    std::vector<wireguard_section> peers{};
    bool in_interface{};
    bool seen_interface{};
};

auto read_wireguard_line(wireguard_document& document, std::string_view raw,
                         std::int64_t line_number, const parse_limits& limits) -> void {
    auto& interface = document.interface;
    auto& peers = document.peers;
    auto& in_interface = document.in_interface;
    auto& seen_interface = document.seen_interface;
    if (line_number > limits.max_lines || std::ssize(raw) > limits.max_line_bytes) {
        fail(parse_error::resource_limit);
    }
    const auto line = trim(raw.substr(0, raw.find_first_of("#;")));
    if (line.empty()) {
        return;
    }
    const auto section_name = lower(line);
    if (section_name == "[interface]") {
        if (seen_interface) {
            fail(parse_error::invalid_document);
        }
        seen_interface = true;
        in_interface = true;
        interface.line = line_number;
        return;
    }
    if (section_name == "[peer]") {
        if (!seen_interface || std::ssize(peers) >= limits.max_nodes) {
            fail(seen_interface ? parse_error::resource_limit : parse_error::invalid_document);
        }
        peers.push_back({.line = line_number});
        in_interface = false;
        return;
    }
    if (!seen_interface || line.starts_with('[') || (!in_interface && peers.empty())) {
        fail(parse_error::invalid_document);
    }
    auto& section = in_interface ? interface : peers.back();
    const auto equal = line.find('=');
    if (equal == std::string_view::npos || equal == 0) {
        section.invalid = true;
        return;
    }
    if (std::ssize(section.fields) >= limits.max_parameters) {
        fail(parse_error::resource_limit);
    }
    set_conf_field(section, conf_key(trim(line.substr(0, equal)), in_interface),
                   trim(line.substr(equal + 1)));
}

auto parse_wireguard_conf(std::string_view input, const parse_limits& limits,
                          subscription_result& result) -> void {
    // 一个 Interface 提供共享本地身份，每个 Peer 各生成一个独立统一节点。
    // 整份文档结构错误会终止导入，单 Peer 参数错误则携带原始行号单独报告，
    // 不能为了保留其他 Peer 而丢掉所有节点共同依赖的接口配置错误。
    auto document = wireguard_document{};
    auto line_number = std::int64_t{};
    for (const auto raw : text_lines(input, limits)) {
        read_wireguard_line(document, raw, ++line_number, limits);
    }
    const auto& interface = document.interface;
    const auto& peers = document.peers;
    if (!document.seen_interface || peers.empty()) {
        fail(parse_error::invalid_document);
    }
    auto index = std::int64_t{};
    for (const auto& peer : peers) {
        ++index;
        append_document_node(
            result, {.line = peer.line, .index = index, .collection = node_collection::peers},
            [&]() -> node_parse_result { return native_wireguard_node(interface, peer, limits); });
    }
}

// NOLINTEND(readability-redundant-member-init)
// NOLINTEND(abseil-string-find-str-contains)
// NOLINTEND(altera-struct-pack-align)
// NOLINTEND(altera-unroll-loops)
// NOLINTEND(fuchsia-default-arguments-declarations)
// NOLINTEND(fuchsia-default-arguments-calls)
// NOLINTEND(llvm-prefer-static-over-anonymous-namespace)
