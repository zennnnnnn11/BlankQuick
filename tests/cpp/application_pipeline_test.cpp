#include <QTemporaryDir>

#include <QCoreApplication>
#include <blank/nodes/node_input.hpp>
#include <blank/overrides/override_manager.hpp>
#include <blank/runtime/configuration_preparation.hpp>
#include <blank/runtime/runtime_session.hpp>
#include <blank/runtime_config/settings_storage.hpp>
#include <blank/telemetry/traffic_controller.hpp>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}

ActivationInput makeInput() {
    constexpr std::string_view fixture{R"yaml(proxies:
  - name: sample
    type: ss
    server: example.com
    port: 443
    cipher: aes-128-gcm
    password: test-only
)yaml"};
    auto parsed = node_input::parse_subscription(fixture);
    require(parsed.error == node_input::parse_error::none, "Fixture parsing failed");
    const auto pool =
        std::make_shared<const std::vector<node_model::unified_node>>(std::move(parsed.nodes));
    auto assembled = profile_payload::try_assemble_minimal_profile(pool, fixture);
    require(assembled.error == profile_payload::assembly_error::none, "Fixture assembly failed");
    return {.material = std::move(assembled.profile)};
}

void checkScriptPipeline() {
    const runtime_config::runtime_context context{
        .data_directory = "Z:/blank-quick-application-test",
        .controller = "127.0.0.1:19090",
        .secret = std::string(64, 'a'),
    };
    auto input = makeInput();
    input.patches = {
        {.ext = "yaml", .content = "ipv6: true\n"},
        {.ext = "js",
         .content = "function main(config) { console.log(config.ipv6); config.ipv6 = !config.ipv6; "
                    "return config; }"},
    };
    const auto result = composeActivation(input, context);
    require(result.error == runtime_config::configuration_error::none && !result.effective.ipv6 &&
                result.patch_reports.size() == 2 &&
                result.patch_reports.back().logs == std::vector<std::string>{"true"},
            "Script did not observe the preceding YAML patch");
    const auto pure =
        runtime_config::generate(input.material, input.settings, context, input.patches);
    require(pure.error == runtime_config::configuration_error::patch_failed &&
                pure.config_json.empty(),
            "The data library executed a JavaScript patch");

    input.patches = {
        {.ext = "js",
         .content = "function main(config) { config.secret = 'changed'; return config; }"}};
    require(composeActivation(input, context).error ==
                runtime_config::configuration_error::protected_field,
            "Script output bypassed the final configuration gate");

    input.patches = {{.ext = "js", .content = "function main(config) { while (true) {} }"}};
    auto limits = runtime_config::generation_limits{};
    limits.patches.script_timeout = std::chrono::milliseconds{25};
    const auto timed = composeActivation(input, context, limits);
    require(timed.error == runtime_config::configuration_error::patch_failed &&
                timed.config_json.empty() && timed.patch_reports.size() == 1 &&
                timed.patch_reports.front().error == overrides::override_error::timeout,
            "The script watchdog failed to stop an infinite loop");

    input.patches = {
        {.ext = "js", .content = "function main(config) { throw new Error('fixture'); }"}};
    const auto failed = composeActivation(input, context);
    require(failed.patch_reports.size() == 1 &&
                failed.patch_reports.front().error == overrides::override_error::script_error,
            "Script errors were not reported");
    input.patches.clear();
    require(composeActivation(input, context).error == runtime_config::configuration_error::none,
            "A failed script mutated the source material");
}

void checkTelemetryRouting() {
    TrafficController traffic;
    auto requests = std::vector<std::pair<bool, quint64>>{};
    QObject::connect(&traffic, &TrafficController::serviceSubscriptionRequested, &traffic,
                     [&requests](bool enabled, quint64 generation) {
                         requests.emplace_back(enabled, generation);
                     });
    traffic.updateContext({.ready = true, .serviceManaged = true});
    traffic.setEnabled(true);
    require(requests.size() == 1 && requests.front().first,
            "Subscription intent was not routed to the coordinator");
    traffic.receiveServiceMessage(QStringLiteral(R"({"up":12,"down":34})"));
    require(traffic.connected() && traffic.upload() == 12 && traffic.download() == 34,
            "Service traffic was not decoded");
    const auto oldGeneration = requests.front().second;
    traffic.updateContext({});
    traffic.updateContext({.ready = true, .serviceManaged = true});
    traffic.receiveServiceMessage(QStringLiteral(R"({"up":56,"down":78})"));
    traffic.serviceRequestFinished(oldGeneration, false);
    require(traffic.connected() && traffic.upload() == 56,
            "An old subscription reply invalidated the new session");
    traffic.setEnabled(false);
    require(!requests.back().first && !traffic.connected(),
            "Disabling telemetry did not request an unsubscribe");
}

void checkSettingsStorage() {
    const QTemporaryDir temporary;
    require(temporary.isValid(), "Temporary storage directory failed");
    const runtime_config::settings_manager manager{{.directory = temporary.path().toStdWString()}};
    const auto initial = manager.load();
    require(initial.value.has_value() && initial.value->revision.empty(),
            "Empty settings state changed");
    const auto saved = manager.save(runtime_config::client_settings{}, {});
    require(saved.value.has_value() && !saved.value->revision.empty(),
            "Settings publication failed");
    require(manager.save(runtime_config::client_settings{}, {}).error ==
                runtime_config::settings_error::conflict,
            "A stale settings revision overwrote committed settings");
    const auto loaded = manager.load();
    require(loaded.value.has_value() && loaded.value->revision == saved.value->revision,
            "Settings adapter did not preserve the revision");
}

void checkOverrideStorage() {
    const QTemporaryDir temporary;
    require(temporary.isValid(), "Temporary override directory failed");
    const overrides::override_manager manager{{.directory = temporary.path().toStdWString()}};
    const auto saved =
        manager.create({.name = "fixture", .ext = "yaml", .content = "ipv6: true\n"});
    require(saved.error == overrides::override_error::none && !saved.value.item.id.empty(),
            "Override publication failed");
    const auto bound = manager.set_profile_binding(
        {.profile_id = "fixture-profile", .override_ids = {saved.value.item.id}});
    require(bound.error == overrides::override_error::none && !bound.value.revision.empty(),
            "Override binding failed");
    const auto selected = manager.load_selection("fixture-profile");
    require(selected.error == overrides::override_error::none && selected.value.items.size() == 1 &&
                selected.value.items.front().revision == saved.value.revision &&
                selected.value.items.front().item.content == saved.value.item.content,
            "Stored override selection lost its revision or content");
    require(manager.update(saved.value.item, {}).error == overrides::override_error::conflict,
            "A stale override revision overwrote committed content");
}

void checkSessionAdmission() {
    auto closed = false;
    RuntimeSession session{{.executable = QStringLiteral("unused-mihomo.exe")}};
    const auto rejected = session.submitActivation(makeInput());
    require(rejected.rejection == ActivationRejection::NotReady && !rejected.accepted(),
            "An unstarted kernel accepted an activation");
    QObject::connect(&session, &RuntimeSession::shutdownFinished, &session,
                     [&closed] { closed = true; });
    session.shutdown();
    require(closed && session.status().lifecycle == RuntimeLifecycle::Closed,
            "An idle session failed to finish shutdown");
    require(session.submitActivation(makeInput()).rejection == ActivationRejection::ShuttingDown,
            "A closed session accepted new work");
}
} // namespace

int main(int argc, char* argv[]) {
    try {
        const QCoreApplication application{argc, argv};
        checkScriptPipeline();
        checkTelemetryRouting();
        checkSettingsStorage();
        checkOverrideStorage();
        checkSessionAdmission();
        std::cout << "Script ordering, watchdog, telemetry routing and storage contracts passed.\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
