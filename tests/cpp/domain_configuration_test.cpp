#include <array>
#include <blank/config/service_config_model.hpp>
#include <blank/nodes/node_input.hpp>
#include <blank/overrides/override_dom.hpp>
#include <blank/overrides/override_engine.hpp>
#include <blank/overrides/override_types.hpp>
#include <blank/profiles/minimal_profile.hpp>
#include <blank/runtime_config/runtime_config.hpp>
#include <blank/runtime_config/settings_codec.hpp>
#include <blank/shared/ip_literal.hpp>
#include <blank/subscriptions/subscription_response.hpp>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

// NOLINTBEGIN(llvm-prefer-static-over-anonymous-namespace,fuchsia-default-arguments-calls)
namespace {
static_assert(blank::net::is_ip_literal("1.1.1.1"));
static_assert(blank::net::is_ip_literal("2001:db8::1"));
static_assert(blank::net::is_ip_literal("::ffff:192.0.2.1"));
static_assert(blank::net::is_ip_literal("fe80::1%ethernet"));
static_assert(!blank::net::is_ip_literal("256.1.1.1"));
static_assert(!blank::net::is_ip_literal("1.1.1.1."));
static_assert(!blank::net::is_ip_literal("1::2::3"));
static_assert(!blank::net::is_ip_literal(":1::"));
static_assert(!blank::net::is_ip_literal("::1:"));
static_assert(!blank::net::is_ip_literal("1:2:3:4:5:6:7:8::"));
static_assert(!blank::net::is_ip_literal("example.com"));

constexpr std::string_view fixture{R"yaml(proxies:
  - name: sample
    type: ss
    server: example.com
    port: 443
    cipher: aes-128-gcm
    password: test-only
)yaml"};

auto require(bool condition, std::string_view message) -> void {
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}

auto check_override_yaml_tags() -> void {
    const auto cases = std::to_array<std::pair<std::string_view, std::string_view>>({
        {"value: !!str 42", R"({"value":"42"})"},
        {"value: !<tag:yaml.org,2002:str> 42", R"({"value":"42"})"},
        {"value: !!null null", R"({"value":null})"},
        {"value: !!bool true", R"({"value":true})"},
        {"value: !!int 42", R"({"value":42})"},
        {"value: !!float 42.5", R"({"value":42.5})"},
        {"value: !!map {nested: 1}", R"({"value":{"nested":1}})"},
        {"value: !!seq [1, 2]", R"({"value":[1,2]})"},
        {"value: 'true'", R"({"value":"true"})"},
    });
    for (const auto& entry : cases) {
        const auto patches = std::array{
            overrides::override_item{.ext = "yaml", .content = std::string{entry.first}}};
        const auto result = overrides::apply_overrides("{}", patches);
        require(result.error == overrides::override_error::none &&
                    overrides::dom::document::parse(result.config_json) ==
                        overrides::dom::document::parse(entry.second),
                "YAML standard tag fixture failed: " + std::string{entry.first} +
                    ", error=" + std::to_string(static_cast<int>(result.error)));
    }
    for (const auto yaml : {"value: !unknown 42", "value: !!bool 42", "value: !!int 1.5",
                            "value: !<tag:example.invalid,2002:str> text"}) {
        const auto patches = std::array{overrides::override_item{.ext = "yaml", .content = yaml}};
        require(overrides::apply_overrides("{}", patches).error ==
                    overrides::override_error::invalid_document,
                "Unknown or mismatched YAML tag was accepted");
    }
}

auto check_override_serialized_size() -> void {
    using overrides::dom::document;
    // dump() is the independent byte oracle in this regression: raw UTF-8 bytes,
    // escaped keys/control characters and formatted numbers must count exactly.
    const auto documents = std::array{
        document::object(),
        document{{"payload", std::string(8192, 'a')}},
        document{{"quote\"\\\n", std::string{"\"\\\b\f\n\r\t"} + '\0' + '\x01'}},
        document{{"中文", "café / 😀"}, {"empty", ""}},
        document{{"signed", std::numeric_limits<std::int64_t>::min()},
                 {"unsigned", std::numeric_limits<std::uint64_t>::max()},
                 {"negative-zero", -0.0},
                 {"tiny", std::numeric_limits<double>::min()},
                 {"large", std::numeric_limits<double>::max()}},
        document{{"nested", document::array({nullptr, true, false, {{"value", "text"}}})}},
    };
    for (const auto& value : documents) {
        auto options = overrides::engine_options{};
        options.max_document_bytes = std::ssize(value.dump());
        const auto exact = overrides::dom::apply_document(value, {}, options);
        require(exact.error == overrides::override_error::none && exact.value == value,
                "Exact compact JSON byte limit rejected an unchanged document");
        --options.max_document_bytes;
        const auto exceeded = overrides::dom::apply_document(value, {}, options);
        require(exceeded.error == overrides::override_error::resource_limit &&
                    exceeded.stage == overrides::dom::failure_stage::input,
                "Serialized byte-limit failure was swallowed or misclassified");
    }
    for (const auto& value :
         {document{{"value", std::string{"\xc3"}}}, document{{std::string{"\xc3"}, "value"}},
          document{{"value", std::numeric_limits<double>::infinity()}},
          document{{"value", std::numeric_limits<double>::quiet_NaN()}}}) {
        require(overrides::dom::apply_document(value, {}, {}).error ==
                    overrides::override_error::invalid_document,
                "Serialized-size validation lost strict UTF-8 or finite-number checks");
    }
}

auto check_override_structure_limits() -> void {
    using overrides::dom::document;
    auto options = overrides::engine_options{};
    const auto value = document{{"values", document::array({0, 1, 2, 3, 4, 5, 6, 7})}};
    options.max_values = 10; // Root object + array + eight scalar values.
    require(overrides::dom::apply_document(value, {}, options).error ==
                overrides::override_error::none,
            "Exact value budget rejected a valid document");
    --options.max_values;
    require(overrides::dom::apply_document(value, {}, options).error ==
                overrides::override_error::resource_limit,
            "Value budget accepted an additional scalar");

    // DFS visits the last array first. Its siblings are already queued and must
    // share the value budget with the children it is about to enqueue.
    auto wide = document::object();
    for (auto index = 0; index < 8; ++index) {
        wide[std::to_string(index)] = nullptr;
    }
    wide["last"] = document::array({0, 1, 2, 3, 4, 5, 6, 7});
    options.max_values = 10;
    require(overrides::dom::apply_document(wide, {}, options).error ==
                overrides::override_error::resource_limit,
            "Queued siblings were omitted from the wide-tree value budget");
    options.max_values = 18;
    require(overrides::dom::apply_document(wide, {}, options).error ==
                overrides::override_error::none,
            "Wide-tree budget rejected an exact fit");
    options.max_depth = 3;
    require(overrides::dom::apply_document(value, {}, options).error ==
                overrides::override_error::none,
            "Exact depth budget rejected a scalar at the last permitted level");
    options.max_depth = 2;
    require(overrides::dom::apply_document(value, {}, options).error ==
                overrides::override_error::resource_limit,
            "Depth budget accepted a scalar beyond its last permitted level");
}

auto check_override_patch_budgets() -> void {
    const auto patches = std::array{
        overrides::override_item{.ext = "yaml", .content = "values+: [one]"},
        overrides::override_item{.ext = "yaml", .content = "values+: [two]"},
    };
    auto options = overrides::engine_options{};
    // The patch's "values+" key adds one byte to its standalone JSON form.
    options.max_document_bytes = std::ssize(std::string_view{R"({"values":["one"]})"}) + 1;
    const auto bytes = overrides::apply_overrides(R"({"values":[]})", patches, options);
    require(bytes.error == overrides::override_error::resource_limit && bytes.config_json.empty() &&
                bytes.reports.size() == 2 &&
                bytes.reports.front().error == overrides::override_error::none &&
                bytes.reports.back().error == overrides::override_error::resource_limit,
            "Accumulated patch bytes bypassed validation or retained a partial output");

    options = {};
    options.max_values = 3; // The first append fits; the second introduces value four.
    const auto values = overrides::apply_overrides(R"({"values":[]})", patches, options);
    require(values.error == overrides::override_error::resource_limit &&
                values.config_json.empty() && values.reports.size() == 2 &&
                values.reports.back().error == overrides::override_error::resource_limit,
            "Accumulated patch values bypassed the shared node budget");
}

auto check_configuration() -> void {
    auto parsed = node_input::parse_subscription(fixture);
    require(parsed.error == node_input::parse_error::none, "Fixture parsing failed");
    const auto material =
        profile_payload::assemble_minimal_profile(std::move(parsed.nodes), fixture);
    // This directory need not exist. A pure generator must not create or inspect it.
    const auto context = runtime_config::runtime_context{
        .data_directory = "Z:/blank-quick-data-only-test",
        .controller = "127.0.0.1:19090",
        .secret = std::string(64, 'a'),
    };
    const auto settings = runtime_config::client_settings{};
    const auto first = runtime_config::generate(material, settings, context);
    const auto second = runtime_config::generate(material, settings, context);
    require(first.error == runtime_config::configuration_error::none &&
                first.config_json == second.config_json,
            "Identical inputs produced different configurations");

    const auto patches = std::array{
        overrides::override_item{.ext = "yaml", .content = "ipv6: true\n"},
        overrides::override_item{.ext = "yaml", .content = "ipv6: false\n"},
    };
    const auto applied = runtime_config::generate(material, settings, context, patches);
    require(applied.error == runtime_config::configuration_error::none && !applied.effective.ipv6 &&
                applied.patch_reports.size() == 2,
            "Sequential YAML patch order changed");

    const auto invalid = std::array{
        overrides::override_item{.ext = "yaml", .content = "external-controller: 0.0.0.0:8080\n"},
    };
    const auto rejected = runtime_config::generate(material, settings, context, invalid);
    require(rejected.error == runtime_config::configuration_error::protected_field &&
                rejected.config_json.empty() && rejected.effective.mixed_port == 0,
            "Rejected configuration retained applicable output");

    for (const auto server : {"1.1.1.1", "2001:db8::1", "tls://[2001:db8::1]:853", "1.1.1.1:53"}) {
        auto selected = settings;
        selected.dns.bootstrap = {server};
        require(runtime_config::generate(material, selected, context).error ==
                    runtime_config::configuration_error::none,
                "An IP bootstrap resolver was rejected");
    }
    for (const auto server :
         {"dns.example.com", "256.1.1.1", ":1::", "https://dns.example.com/dns-query"}) {
        auto selected = settings;
        selected.dns.bootstrap = {server};
        require(runtime_config::generate(material, selected, context).error !=
                    runtime_config::configuration_error::none,
                "A nonliteral bootstrap resolver was accepted");
    }
}

auto check_settings_codec() -> void {
    const auto encoded = runtime_config::encode_settings(runtime_config::client_settings{});
    require(encoded.error == runtime_config::configuration_error::none && encoded.value.has_value(),
            "Settings encoding failed");
    const auto decoded = runtime_config::decode_settings(*encoded.value);
    require(decoded.value.has_value(), "Settings decoding failed");
    const auto encoded_again = runtime_config::encode_settings(*decoded.value);
    require(decoded.error == runtime_config::configuration_error::none &&
                encoded_again.value == encoded.value,
            "Settings changed during a codec round trip");
    auto corrupt = *encoded.value;
    corrupt.erase("mixed_port");
    require(runtime_config::decode_settings(corrupt).error !=
                runtime_config::configuration_error::none,
            "Incomplete settings were accepted");
}

auto check_subscription_response() -> void {
    const auto accepted =
        subscription_data::decode_response({.body = std::string{fixture}, .status = 200}, {});
    require(accepted.error == subscription_data::fetch_error::none &&
                accepted.subscription.nodes.size() == 1 && accepted.raw_body == fixture,
            "Received subscription bytes did not decode offline");
    const auto unchanged = subscription_data::decode_response({.status = 304}, {});
    require(unchanged.not_modified && unchanged.raw_body.empty(),
            "HTTP 304 lost its cache semantics");
    const auto html =
        subscription_data::decode_response({.body = " <HTML>login</HTML>", .status = 200}, {});
    require(html.error == subscription_data::fetch_error::unexpected_content,
            "An HTML login page was treated as a subscription");
    require(subscription_data::decode_response({.status = 407}, {}).error ==
                subscription_data::fetch_error::proxy_failure,
            "Proxy authentication failure changed its route retry classification");
    auto limits = subscription_data::response_options{};
    limits.headers.max_bytes = 1;
    auto aborted = false;
    try {
        static_cast<void>(subscription_data::decode_response(
            {.headers = {{"Content-Type", "text/plain"}}, .status = 304}, limits));
    } catch (const subscription_data::response_failure& failure) {
        aborted = failure.code() == subscription_data::fetch_error::headers_too_large;
    }
    require(aborted, "Oversized response headers did not abort the fetch");
}

auto check_service_composition() -> void {
    const auto accepted = composeServiceConfig(QStringLiteral("{}"), 7890, {});
    require(accepted.error.isEmpty(), "Service defaults could not be composed without I/O");
    const auto rejected = composeServiceConfig(
        QStringLiteral(R"({"rule-providers":{"sample":{"type":"file","path":"../escape"}}})"), 7890,
        {});
    require(!rejected.error.isEmpty(), "Provider traversal escaped lexical admission");
}
} // namespace

auto main() -> int {
    try {
        // Intentionally no QCoreApplication, temporary directory or real kernel.
        check_configuration();
        check_settings_codec();
        check_service_composition();
        check_subscription_response();
        check_override_yaml_tags();
        check_override_serialized_size();
        check_override_structure_limits();
        check_override_patch_budgets();
        std::cout << "Data-only configuration and codec contracts passed.\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
// NOLINTEND(llvm-prefer-static-over-anonymous-namespace,fuchsia-default-arguments-calls)
