#include <QtCore/QCoreApplication>
#include <QtCore/QTemporaryDir>

#include <array>
#include <blank/nodes/node_input.hpp>
#include <blank/overrides/override_types.hpp>
#include <blank/profiles/profile_runtime.hpp>
#include <blank/profiles/profile_storage.hpp>
#include <blank/runtime_config/runtime_config.hpp>
#include <blank/runtime_config/runtime_context.hpp>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>

// Repository policy uses anonymous namespaces and standard-library default arguments.
// NOLINTBEGIN(llvm-prefer-static-over-anonymous-namespace,fuchsia-default-arguments-calls)
namespace {
constexpr std::string_view fixture{R"yaml(proxies:
  - name: alpha
    type: ss
    server: example.com
    port: 443
    cipher: aes-128-gcm
    password: fixture-secret
    plugin: v2ray-plugin
    plugin-opts:
      mode: websocket
      host: cdn.example.com
      tls: true
      mux: true
  - name: beta
    type: vmess
    server: example.org
    port: 8443
    uuid: 00000000-0000-4000-8000-000000000001
    alterId: 0
    cipher: auto
    network: ws
    tls: true
    ws-opts:
      path: /fixture
      headers:
        Host: ws.example.org
  - name: gamma
    type: trojan
    server: example.net
    port: 443
    password: fixture-password
    alpn: []
  - name: delta
    type: wireguard
    ip: 10.0.0.2
    private-key: AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=
    peers:
      - server: wg.example.com
        port: 51820
        public-key: AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=
        allowed-ips: [0.0.0.0/0]
proxy-groups:
  - name: choice
    type: select
    proxies: [alpha, beta, gamma, delta]
rules:
  - MATCH,choice
)yaml"};

auto require(bool condition, std::string_view message) -> void {
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}

auto check_activation(const std::filesystem::path& directory) -> void {
    const auto parsed = node_input::parse_subscription(fixture);
    require(parsed.error == node_input::parse_error::none && parsed.nodes.size() == 4,
            "Clash subscription import failed");
    const auto manager = profile_storage::profile_manager{{.directory = directory / "profiles"}};
    const auto saved = manager.save_new({
        .settings = {.name = "fixture", .preserve_original_rules = true},
        .raw = fixture,
        .nodes = parsed.nodes,
    });
    require(saved.error == profile_storage::storage_error::none, "Profile publication failed");
    const auto loaded = profile_runtime::load_session(manager, saved.value.id);
    if (!loaded.value.has_value()) {
        throw std::runtime_error{"Profile cache reload failed"};
    }
    const auto activation = loaded.value->activate();
    require(activation.value != nullptr, "Stored profile assembly failed");
    const auto context = runtime_config::make_runtime_context(directory / "runtime", 19090);
    const auto settings = runtime_config::client_settings{};
    const auto patch = std::array{
        overrides::override_item{
            .id = "00000000000000000000000000000001",
            .ext = "yaml",
            .content = "ipv6: true\n",
        },
    };
    const auto generated =
        runtime_config::generate(activation.value->profile, settings, context, patch);
    require(generated.error == runtime_config::configuration_error::none,
            "Runtime generation failed");
    const auto document = nlohmann::json::parse(generated.config_json);
    require(document.at("ipv6").get<bool>(), "YAML override was not applied");
    const auto& proxies = document.at("proxies");
    require(proxies.size() == 4, "Cached nodes were lost during export");
    require(proxies.at(0).at("plugin-opts").at("host") == "cdn.example.com",
            "Plugin alternative did not survive cache/export");
    require(proxies.at(1).at("ws-opts").at("headers").at("Host") == "ws.example.org",
            "Nested transport fields did not survive cache/export");
    require(proxies.at(2).at("alpn").is_array() && proxies.at(2).at("alpn").empty(),
            "An explicitly empty optional field lost its presence");
    require(proxies.at(3).at("peers").at(0).at("server") == "wg.example.com",
            "Structured peer arrays did not survive import/cache/export");
    require(document.at("rules").at(0) == "MATCH,choice", "Original rules were not preserved");
    const auto invalid = std::array{
        overrides::override_item{
            .id = "00000000000000000000000000000002",
            .ext = "yaml",
            .content = "external-controller: 0.0.0.0:9999\n",
        },
    };
    const auto rejected =
        runtime_config::generate(activation.value->profile, settings, context, invalid);
    require(rejected.error == runtime_config::configuration_error::protected_field &&
                rejected.config_json.empty(),
            "Invalid controller override produced an applicable candidate");
    const auto again = runtime_config::generate(activation.value->profile, settings, context);
    require(again.error == runtime_config::configuration_error::none &&
                !nlohmann::json::parse(again.config_json).at("ipv6").get<bool>(),
            "A failed/transient override mutated the saved profile");
    {
        auto stray_generation = std::ofstream{directory / "profiles" / "profiles" / saved.value.id /
                                              std::filesystem::path{L"\U0001F680"}};
        stray_generation << "stray";
        auto stray_profile = std::ofstream{directory / "profiles" / "profiles" /
                                           std::filesystem::path{L"\U0001F680"}};
        stray_profile << "stray";
    }
    const auto collected = manager.collect_garbage();
    require(collected.error == profile_storage::storage_error::none,
            "Unicode stray entry broke profile garbage collection");
    const auto replaced = manager.replace({
        .id = saved.value.id,
        .expected_revision = saved.value.revision,
        .raw = fixture,
        .nodes = parsed.nodes,
    });
    require(replaced.error == profile_storage::storage_error::none &&
                manager.collect_garbage().error == profile_storage::storage_error::none,
            "Profile replace and generation cleanup failed with Unicode stray entries");
}
} // namespace

auto main(int argc, char* argv[]) -> int {
    try {
        const QCoreApplication application{argc, argv, QCoreApplication::ApplicationFlags};
        const QTemporaryDir temporary{};
        if (!temporary.isValid()) {
            return EXIT_FAILURE;
        }
        check_activation(std::filesystem::path{temporary.path().toStdWString()});
        std::cout << "Import, cache reload, activation, export and override isolation passed.\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        try {
            std::cerr << error.what() << '\n';
        } catch (const std::exception&) {
            return EXIT_FAILURE;
        }
        return EXIT_FAILURE;
    }
}
// NOLINTEND(llvm-prefer-static-over-anonymous-namespace,fuchsia-default-arguments-calls)
