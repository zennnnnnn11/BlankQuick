#include <array>
#include <blank/config/service_config_model.hpp>
#include <blank/nodes/node_input.hpp>
#include <blank/profiles/minimal_profile.hpp>
#include <blank/runtime_config/runtime_config.hpp>
#include <blank/runtime_config/settings_codec.hpp>
#include <blank/shared/ip_literal.hpp>
#include <blank/subscriptions/subscription_response.hpp>
#include <cstdlib>
#include <exception>
#include <iostream>
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
        std::cout << "Data-only configuration and codec contracts passed.\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
// NOLINTEND(llvm-prefer-static-over-anonymous-namespace,fuchsia-default-arguments-calls)
