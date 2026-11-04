// Sing-box names are translated explicitly; they are not Mihomo key aliases.
// ISO C++20 CPU adapters use the same local style exceptions as node_input.cpp.
// NOLINTBEGIN(llvm-prefer-static-over-anonymous-namespace)
// NOLINTBEGIN(fuchsia-default-arguments-calls)
// NOLINTBEGIN(fuchsia-default-arguments-declarations)
// NOLINTBEGIN(altera-unroll-loops)
// NOLINTBEGIN(altera-struct-pack-align)
// NOLINTBEGIN(abseil-string-find-str-contains)
// NOLINTBEGIN(readability-redundant-member-init)

struct singbox_mapping { // NOLINT(bugprone-exception-escape)
    options source{};
    options target{};
};

// Source and destination are deliberately paired at each conversion call.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
auto translate(singbox_mapping& mapping, std::string_view from, std::string_view destination)
    -> void {
    if (const auto* value = find_option(mapping.source, from)) {
        set_option(mapping.target, destination, *value);
    }
}

auto reject_effective(options& source, std::string_view key) -> void {
    const auto* value = find_option(source, key);
    if (value == nullptr || value->is_null() || *value == false || value->empty()) {
        return;
    }
    if (value->is_string() && value->get_ref<const std::string&>().empty()) {
        return;
    }
    fail(parse_error::unsupported_parameter);
}

[[nodiscard]] auto enabled_option(options& source, std::string_view key) -> bool {
    auto result = false;
    read_option(source, key, result);
    return result;
}

[[nodiscard]] auto joined_strings(const json& value, std::string_view separator) -> std::string {
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (!value.is_array()) {
        fail(parse_error::invalid_parameter);
    }
    auto result = std::string{};
    for (const auto& entry : value) {
        if (!entry.is_string()) {
            fail(parse_error::invalid_parameter);
        }
        if (!result.empty()) {
            result += separator;
        }
        result += entry.get<std::string>();
    }
    return result;
}

auto singbox_tls(singbox_mapping& mapping, uri_protocol protocol) -> void {
    const auto* tls = peek_option(mapping.source, "tls");
    if (tls == nullptr) {
        return;
    }
    if (!tls->is_object()) {
        fail(parse_error::invalid_parameter);
    }
    const auto active = enabled_option(mapping.source, "tls.enabled");
    set_option(mapping.target, "tls", active);
    if (!active) {
        static_cast<void>(find_option(mapping.source, "tls"));
        return;
    }
    const auto engine = option_text(mapping.source, "tls.engine");
    if (!engine.empty() && engine != "go") {
        fail(parse_error::unsupported_parameter);
    }
    // CA stores, SPKI pins, and TLS-version constraints have no per-node
    // equivalent in the audited Mihomo model. Never confuse them with mTLS.
    for (const auto* key : {
             "tls.certificate",
             "tls.certificate_path",
             "tls.certificate_sha256",
             "tls.certificate_public_key_sha256",
             "tls.client_certificate_path",
             "tls.client_key_path",
             "tls.min_version",
             "tls.max_version",
             "tls.cipher_suites",
             "tls.curve_preferences",
             "tls.fragment",
             "tls.record_fragment",
             "tls.spoof",
         }) {
        reject_effective(mapping.source, key);
    }
    translate(mapping, "tls.server_name", "servername");
    translate(mapping, "tls.insecure", "skip-cert-verify");
    translate(mapping, "tls.alpn", "alpn");
    translate(mapping, "tls.handshake_timeout", "handshake-timeout");
    if (enabled_option(mapping.source, "tls.disable_sni")) {
        if (protocol != uri_protocol::tuic) {
            fail(parse_error::unsupported_parameter);
        }
        set_option(mapping.target, "disable-sni", true);
    }
    for (const auto& field : std::to_array<std::pair<std::string_view, std::string_view>>(
             {{"tls.client_certificate", "certificate"}, {"tls.client_key", "private-key"}})) {
        if (const auto* value = find_option(mapping.source, field.first)) {
            set_option(mapping.target, field.second, joined_strings(*value, "\n"));
        }
    }
    if (enabled_option(mapping.source, "tls.utls.enabled")) {
        translate(mapping, "tls.utls.fingerprint", "client-fingerprint");
    } else {
        static_cast<void>(find_option(mapping.source, "tls.utls"));
    }
    if (enabled_option(mapping.source, "tls.reality.enabled")) {
        translate(mapping, "tls.reality.public_key", "reality-opts.public-key");
        translate(mapping, "tls.reality.short_id", "reality-opts.short-id");
        if (option_text(mapping.target, "reality-opts.public-key").empty()) {
            fail(parse_error::missing_credential);
        }
    } else {
        static_cast<void>(find_option(mapping.source, "tls.reality"));
    }
    if (enabled_option(mapping.source, "tls.ech.enabled")) {
        reject_effective(mapping.source, "tls.ech.config_path");
        set_option(mapping.target, "ech-opts.enable", true);
        translate(mapping, "tls.ech.query_server_name", "ech-opts.query-server-name");
        if (const auto* value = find_option(mapping.source, "tls.ech.config")) {
            set_option(mapping.target, "ech-opts.config", joined_strings(*value, ""));
        }
    } else {
        static_cast<void>(find_option(mapping.source, "tls.ech"));
    }
}

[[nodiscard]] auto singbox_headers(const json& value) -> json {
    if (!value.is_object()) {
        fail(parse_error::invalid_parameter);
    }
    auto result = json::object();
    for (const auto& [name, item] : value.items()) {
        // HTTPHeader is Listable in Sing-box; WS/Upgrade in Mihomo is scalar.
        if (item.is_array()) {
            if (item.size() != 1) {
                fail(parse_error::unsupported_parameter);
            }
            set_json_member(result, name, scalar_string(item.front()));
        } else {
            set_json_member(result, name, scalar_string(item));
        }
    }
    return result;
}

auto singbox_transport(singbox_mapping& mapping) -> void {
    const auto* transport = peek_option(mapping.source, "transport");
    if (transport == nullptr) {
        return;
    }
    if (!transport->is_object()) {
        fail(parse_error::invalid_parameter);
    }
    const auto kind = lower(option_text(mapping.source, "transport.type"));
    auto prefix = std::string{};
    if (kind == "ws" || kind == "httpupgrade") {
        prefix = "ws-opts.";
        set_option(mapping.target, "network", kind);
        if (const auto* headers = find_option(mapping.source, "transport.headers")) {
            set_option(mapping.target, prefix + "headers", singbox_headers(*headers));
        }
        translate(mapping, "transport.host", prefix + "headers.Host");
        translate(mapping, "transport.max_early_data", prefix + "max-early-data");
        translate(mapping, "transport.early_data_header_name", prefix + "early-data-header-name");
    } else if (kind == "grpc") {
        set_option(mapping.target, "network", "grpc");
        translate(mapping, "transport.service_name", "grpc-opts.grpc-service-name");
    } else if (kind == "http") {
        // Sing-box HTTP uses H2 with TLS and HTTP camouflage without TLS.
        const auto tls = enabled_option(mapping.target, "tls");
        set_option(mapping.target, "network", tls ? "h2" : "tcp");
        if (tls) {
            prefix = "h2-opts.";
            const auto method = option_text(mapping.source, "transport.method");
            if (!method.empty() && method != "PUT") {
                fail(parse_error::unsupported_parameter);
            }
            reject_effective(mapping.source, "transport.headers");
            translate(mapping, "transport.host", prefix + "host");
        } else {
            prefix = "http-opts.";
            set_option(mapping.target, "headerType", "http");
            set_option(mapping.target, prefix + "method",
                       option_text(mapping.source, "transport.method", "PUT"));
            translate(mapping, "transport.headers", prefix + "headers");
            translate(mapping, "transport.host", prefix + "headers.Host");
        }
    } else {
        fail(parse_error::unsupported_parameter);
    }
    if (!prefix.empty()) {
        translate(mapping, "transport.path", prefix + "path");
    }
}

auto singbox_network(singbox_mapping& mapping, uri_protocol protocol) -> void {
    const auto always_udp = protocol == uri_protocol::hysteria2 || protocol == uri_protocol::tuic;
    if (!always_udp) {
        set_option(mapping.target, "udp", true);
    }
    if (const auto* value = find_option(mapping.source, "network")) {
        auto networks = std::vector<std::string>{};
        read_value(*value, networks);
        auto tcp = false;
        auto udp = false;
        for (const auto& network : networks) {
            tcp = tcp || network == "tcp" || network.empty();
            udp = udp || network == "udp" || network.empty();
            if (network != "tcp" && network != "udp" && !network.empty()) {
                fail(parse_error::invalid_parameter);
            }
        }
        if (!tcp || (always_udp && !udp)) {
            fail(parse_error::unsupported_parameter);
        }
        if (!always_udp) {
            set_option(mapping.target, "udp", udp);
        }
    }
}

auto singbox_common(singbox_mapping& mapping) -> void {
    constexpr auto fields = std::to_array<std::pair<std::string_view, std::string_view>>({
        {"server", "server"},
        {"server_port", "port"},
        {"uuid", "uuid"},
        {"password", "password"},
        {"alter_id", "alter-id"},
        {"flow", "flow"},
        {"global_padding", "global-padding"},
        {"authenticated_length", "authenticated-length"},
        {"packet_encoding", "packet-encoding"},
        {"detour", "dialer-proxy"},
        {"bind_interface", "interface-name"},
        {"routing_mark", "routing-mark"},
        {"tcp_fast_open", "tfo"},
        {"tcp_multi_path", "mptcp"},
        {"up_mbps", "up"},
        {"down_mbps", "down"},
        {"bbr_profile", "bbr-profile"},
        {"congestion_control", "congestion-controller"},
        {"udp_relay_mode", "udp-relay-mode"},
        {"udp_over_stream", "udp-over-stream"},
        {"zero_rtt_handshake", "reduce-rtt"},
        {"heartbeat", "heartbeat-interval"},
        {"client_metadata", "client-metadata"},
        {"idle_session_check_interval", "idle-session-check-interval"},
        {"idle_session_timeout", "idle-session-timeout"},
        {"min_idle_session", "min-idle-session"},
    });
    for (const auto& field : fields) {
        translate(mapping, field.first, field.second);
    }
    const auto strategy = option_text(mapping.source, "domain_strategy");
    if (!strategy.empty() && strategy != "as_is") {
        constexpr auto strategies = std::to_array<std::pair<std::string_view, std::string_view>>({
            {"prefer_ipv4", "ipv4-prefer"},
            {"prefer_ipv6", "ipv6-prefer"},
            {"ipv4_only", "ipv4"},
            {"ipv6_only", "ipv6"},
        });
        const auto found =
            std::ranges::find(strategies, strategy, &decltype(strategies)::value_type::first);
        if (found == strategies.end()) {
            fail(parse_error::unsupported_parameter);
        }
        set_option(mapping.target, "ip-version", found->second);
    }
}

auto singbox_multiplex(singbox_mapping& mapping) -> void {
    for (const auto* key :
         {"enabled", "protocol", "max_connections", "min_streams", "max_streams", "padding"}) {
        translate(mapping, std::string{"multiplex."} + key, std::string{"smux."} + key);
    }
    translate(mapping, "multiplex.brutal.enabled", "smux.brutal-opts.enabled");
    translate(mapping, "multiplex.brutal.up_mbps", "smux.brutal-opts.up");
    translate(mapping, "multiplex.brutal.down_mbps", "smux.brutal-opts.down");
}

auto singbox_hysteria(singbox_mapping& mapping) -> void {
    translate(mapping, "stream_receive_window", "max-stream-receive-window");
    translate(mapping, "connection_receive_window", "max-connection-receive-window");
    if (const auto* realm = peek_option(mapping.source, "realm");
        realm != nullptr && !realm->is_null()) {
        if (!realm->is_object()) {
            fail(parse_error::invalid_parameter);
        }
        set_option(mapping.target, "realm-opts.enable", true);
        for (const auto* key : {"server_url", "token", "realm_id", "stun_servers"}) {
            translate(mapping, std::string{"realm."} + key, std::string{"realm-opts."} + key);
        }
        reject_effective(mapping.source, "realm.http_client");
        reject_effective(mapping.source, "realm.port_mapping");
        auto version = std::int64_t{};
        read_option(mapping.source, "realm.ip_version", version);
        if (version != 0) {
            fail(parse_error::unsupported_parameter);
        }
    }
    translate(mapping, "obfs.type", "obfs");
    translate(mapping, "obfs.password", "obfs-password");
    translate(mapping, "obfs.min_packet_size", "obfs-min-packet-size");
    translate(mapping, "obfs.max_packet_size", "obfs-max-packet-size");
    if (const auto* ports = find_option(mapping.source, "server_ports")) {
        auto text = joined_strings(*ports, ",");
        std::ranges::replace(text, ':', '-');
        set_option(mapping.target, "ports", text);
        if (!mapping.target.values.contains("port")) {
            set_option(mapping.target, "port", hopping_first_port(text));
        }
    }
    if (const auto* interval = find_option(mapping.source, "hop_interval")) {
        set_option(mapping.target, "hop-interval", duration_count(*interval, 1));
        if (const auto* maximum = find_option(mapping.source, "hop_interval_max")) {
            set_option(mapping.target, "hop-interval",
                       std::to_string(duration_count(*interval, 1)) + '-' +
                           std::to_string(duration_count(*maximum, 1)));
        }
    }
}

auto singbox_wireguard(singbox_mapping& mapping) -> void {
    translate(mapping, "private_key", "private-key");
    translate(mapping, "peer_public_key", "public-key");
    translate(mapping, "public_key", "public-key");
    translate(mapping, "pre_shared_key", "pre-shared-key");
    translate(mapping, "reserved", "reserved");
    translate(mapping, "mtu", "mtu");
    translate(mapping, "workers", "workers");
    translate(mapping, "local_address", "address");
    translate(mapping, "address", "address");
    if (const auto* peers = find_option(mapping.source, "peers")) {
        if (!peers->is_array() || peers->empty()) {
            fail(parse_error::missing_credential);
        }
        auto mapped = json::array();
        auto keepalive = std::optional<std::int64_t>{};
        for (const auto& object : *peers) {
            auto source = object_options(object, mapping.source.limits);
            auto target = json::object();
            for (const auto* key : {"public_key", "pre_shared_key", "reserved", "allowed_ips"}) {
                if (const auto* value = find_option(source, key)) {
                    set_json_member(target, std::string{key}, *value);
                }
            }
            auto host = option_text(source, "server", option_text(source, "address"));
            set_json_member(target, "server", std::move(host));
            set_json_member(target, "port",
                            option_text(source, "server_port", option_text(source, "port")));
            auto interval = std::int64_t{};
            read_option(source, "persistent_keepalive_interval", interval);
            if (keepalive.has_value() && *keepalive != interval) {
                fail(parse_error::unsupported_parameter);
            }
            keepalive = interval;
            if (!collect_ignored(source).empty()) {
                fail(parse_error::unsupported_parameter);
            }
            mapped.push_back(std::move(target));
        }
        set_option(mapping.target, "peers", mapped);
        set_option(mapping.target, "persistent-keepalive", keepalive.value_or(0));
    }
}

auto singbox_shadowsocks(singbox_mapping& mapping) -> void {
    translate(mapping, "method", "cipher");
    auto plugin = option_text(mapping.source, "plugin");
    const auto arguments = option_text(mapping.source, "plugin_opts");
    if (!arguments.empty()) {
        plugin += ';' + arguments;
    }
    if (!plugin.empty()) {
        set_option(mapping.target, "plugin", plugin);
    }
    if (const auto* uot = find_option(mapping.source, "udp_over_tcp")) {
        if (uot->is_object()) {
            translate(mapping, "udp_over_tcp.enabled", "udp-over-tcp");
            translate(mapping, "udp_over_tcp.version", "udp-over-tcp-version");
        } else {
            set_option(mapping.target, "udp-over-tcp", *uot);
        }
    }
}

auto singbox_security_leftovers(singbox_mapping& mapping) -> void {
    for (const auto& ignored : collect_ignored(mapping.source)) {
        if (ignored.starts_with("tls.") && !ignored.ends_with(".pqsignatureschemesenabled") &&
            !ignored.ends_with(".dynamicrecordsizingdisabled")) {
            reject_effective(mapping.source, ignored);
        }
    }
}

[[nodiscard]] auto singbox_node(const json& object, const parse_limits& limits)
    -> node_parse_result {
    auto mapping =
        singbox_mapping{.source = object_options(object, limits), .target = {.limits = limits}};
    auto type = option_text(mapping.source, "type");
    if (type == "shadowsocks") {
        type = "ss";
    }
    const auto protocol = parse_protocol(type);
    auto name = option_text(mapping.source, "tag");
    singbox_common(mapping);
    singbox_network(mapping, protocol);
    if (protocol == uri_protocol::vless) {
        const auto* encoding = peek_option(mapping.source, "packet_encoding");
        if (encoding != nullptr && encoding->is_string() && encoding->get<std::string>().empty()) {
            fail(parse_error::unsupported_parameter);
        }
    }
    if (protocol == uri_protocol::vmess) {
        translate(mapping, "security", "cipher");
        if (peek_option(mapping.source, "packet_encoding") == nullptr) {
            translate(mapping, "packet_addr", "packet-addr");
        }
    }
    if (protocol == uri_protocol::shadowsocks) {
        singbox_shadowsocks(mapping);
    }
    singbox_tls(mapping, protocol);
    singbox_transport(mapping);
    singbox_multiplex(mapping);
    if (protocol == uri_protocol::hysteria2) {
        singbox_hysteria(mapping);
    }
    if (protocol == uri_protocol::wireguard) {
        singbox_wireguard(mapping);
    }
    singbox_security_leftovers(mapping);
    auto result = assemble(raw_parts(std::move(mapping.target), protocol, std::move(name)), false);
    for (const auto& ignored : collect_ignored(mapping.source)) {
        result.ignored_parameters.push_back("sing-box." + ignored);
    }
    return result;
}

auto extract_singbox_array(const json& array, node_collection collection,
                           const parse_limits& limits, subscription_result& result) -> void {
    if (!array.is_array()) {
        fail(parse_error::invalid_document);
    }
    auto index = std::int64_t{};
    for (const auto& node : array) {
        if (++index > limits.max_nodes || std::ssize(result.nodes) >= limits.max_nodes) {
            fail(parse_error::resource_limit);
        }
        append_document_node(result, {.index = index, .collection = collection},
                             [&]() -> node_parse_result { return singbox_node(node, limits); });
    }
}

auto parse_json_document(std::string_view input, const parse_limits& limits,
                         subscription_result& result) -> void {
    auto root = json{};
    auto malformed_json = false;
    try {
        root = bounded_json(input, limits);
    } catch (const json::parse_error&) {
        malformed_json = true;
    }
    if (malformed_json) {
        if (!input.starts_with('{')) {
            fail(parse_error::invalid_document);
        }
        result.format = subscription_format::clash_yaml;
        parse_clash_yaml(input, limits, result);
        return;
    }
    if (root.is_object() && root.contains("proxies")) {
        result.format = subscription_format::clash_yaml;
        extract_clash_json(root.at("proxies"), limits, result);
        return;
    }
    result.format = subscription_format::sing_box_json;
    if (root.is_array()) {
        extract_singbox_array(root, node_collection::nodes, limits, result);
    } else if (root.is_object() && (root.contains("outbounds") || root.contains("endpoints"))) {
        if (root.contains("outbounds")) {
            extract_singbox_array(root.at("outbounds"), node_collection::outbounds, limits, result);
        }
        if (root.contains("endpoints")) {
            extract_singbox_array(root.at("endpoints"), node_collection::endpoints, limits, result);
        }
    } else if (root.is_object() && root.contains("type")) {
        append_document_node(result, {.index = 1},
                             [&]() -> node_parse_result { return singbox_node(root, limits); });
    } else {
        fail(parse_error::invalid_document);
    }
}

// NOLINTEND(readability-redundant-member-init)
// NOLINTEND(abseil-string-find-str-contains)
// NOLINTEND(altera-struct-pack-align)
// NOLINTEND(altera-unroll-loops)
// NOLINTEND(fuchsia-default-arguments-declarations)
// NOLINTEND(fuchsia-default-arguments-calls)
// NOLINTEND(llvm-prefer-static-over-anonymous-namespace)
