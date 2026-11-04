#include <QtCore/QCoreApplication>
#include <QtCore/QString>
#include <QtQml/QJSEngine>
#include <QtQml/QJSValue>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "override_detail.hpp"
#include "override_engine.hpp"
#include "override_types.hpp"

namespace overrides::detail {
namespace {
// QJSEngine is deliberately local to its calling thread. Only its documented
// thread-safe setInterrupted() is called by the automatically joined watchdog.
// NOLINTBEGIN(llvm-prefer-static-over-anonymous-namespace,altera-struct-pack-align,readability-redundant-member-init,altera-unroll-loops,fuchsia-default-arguments-calls)

constexpr std::string_view bootstrap{R"JS((function(maxLines, maxChars, maxDepth, maxValues) {
    "use strict";
    const parse = JSON.parse, stringify = JSON.stringify;
    const evaluate = eval;
    const create = Object.create, define = Object.defineProperty, setPrototype = Object.setPrototypeOf;
    const keys = Object.keys, symbols = Object.getOwnPropertySymbols;
    const descriptors = Object.getOwnPropertyDescriptors, prototype = Object.getPrototypeOf;
    const objectPrototype = Object.prototype, isArray = Array.isArray;
    const finite = Number.isFinite, safeInteger = Number.isSafeInteger;
    const integer = Number.isInteger, textOf = String;
    const slice = Function.prototype.call.bind(String.prototype.slice);
    const own = Function.prototype.call.bind(Object.prototype.hasOwnProperty);
    const logs = create(null);
    let lineCount = 0, charCount = 0, truncated = false;
    function log() {
        if (lineCount >= maxLines || charCount >= maxChars) { truncated = true; return; }
        let line = "";
        for (let index = 0; index < arguments.length; ++index) {
            let text;
            try { text = textOf(arguments[index]); } catch (_) { text = "[unprintable]"; }
            if (index) line += " ";
            line += slice(text, 0, maxChars - charCount - line.length);
            if (line.length >= maxChars - charCount) { truncated = true; break; }
        }
        define(logs, lineCount++, {value: line, enumerable: true});
        charCount += line.length;
    }
    const consoleObject = create(null);
    for (const name of ["log", "info", "warn", "error", "debug"])
        define(consoleObject, name, {value: log, enumerable: true});
    define(globalThis, "console", {value: consoleObject});
    function message(error) {
        try { return slice(textOf(error), 0, maxChars); }
        catch (_) { return "Script threw an unprintable value"; }
    }
    function run(source, input) {
        let output;
        try {
            const main = evaluate(source);
            if (typeof main !== "function") throw "Missing main(config)";
            output = main(parse(input));
        }
        catch (error) { return [false, "script", message(error)]; }
        try {
            let count = 0;
            const path = create(null);
            let pathSize = 0;
            function clone(value, depth) {
                if (++count > maxValues || depth >= maxDepth) throw "Output budget exceeded";
                if (value === null || typeof value === "string" || typeof value === "boolean")
                    return value;
                if (typeof value === "number") {
                    if (!finite(value) || (integer(value) && !safeInteger(value)))
                        throw "Nonfinite or unsafe integer output";
                    return value;
                }
                if (typeof value !== "object") throw "Output must contain only JSON data";
                const array = isArray(value);
                const proto = prototype(value);
                if (!array && proto !== objectPrototype && proto !== null)
                    throw "Output must contain plain objects";
                if (symbols(value).length) throw "Symbol keys are not JSON data";
                for (let index = 0; index < pathSize; ++index)
                    if (path[index] === value) throw "Cyclic output";
                path[pathSize++] = value;
                const desc = descriptors(value), names = keys(value);
                const result = array ? [] : create(null);
                // Null prototypes prevent user toJSON/prototype changes from hiding data.
                if (array) setPrototype(result, null);
                if (names.length > maxValues - count) throw "Output budget exceeded";
                if (array && names.length !== value.length) throw "Sparse or decorated array";
                for (let index = 0; index < names.length; ++index) {
                    const key = names[index], entry = desc[key];
                    if (!own(entry, "value")) throw "Accessors are not JSON data";
                    if (array && key !== textOf(index)) throw "Decorated array";
                    define(result, key, {value: clone(entry.value, depth + 1), enumerable: true,
                                         writable: true, configurable: true});
                }
                --pathSize;
                return result;
            }
            if (output === null || typeof output !== "object" || isArray(output))
                throw "main(config) must return a configuration object";
            return [true, stringify(clone(output, 0)), ""];
        } catch (error) { return [false, "result", message(error)]; }
    }
    return {run: run, logAt: function(index) { return logs[index]; },
            count: function() { return lineCount; }, cut: function() { return truncated; }};
}))JS"};

[[nodiscard]] auto qt_text(std::string_view text) -> QString {
    return QString::fromUtf8(std::to_address(text.begin()), std::ssize(text));
}

[[nodiscard]] auto js_failed(QJSEngine& engine, const QJSValue& value) -> bool {
    // evaluate() catches syntax exceptions itself; hasError() alone is insufficient.
    const auto pending = engine.hasError();
    if (pending) {
        static_cast<void>(engine.catchError());
    }
    return pending || value.isError();
}

auto require_safe_numbers(const document& configuration) -> void {
    constexpr std::int64_t maximum_safe_integer{9007199254740991};
    auto pending = std::vector<std::reference_wrapper<const document>>{std::cref(configuration)};
    while (!pending.empty()) {
        const auto& value = pending.back().get();
        pending.pop_back();
        if (value.is_number_unsigned()) {
            if (value.get<std::uint64_t>() > maximum_safe_integer) {
                fail(override_error::invalid_script_result);
            }
        } else if (value.is_number_integer()) {
            const auto number = value.get<std::int64_t>();
            if (number < -maximum_safe_integer || number > maximum_safe_integer) {
                fail(override_error::invalid_script_result);
            }
        }
        if (value.is_structured()) {
            std::ranges::transform(
                value, std::back_inserter(pending),
                [](const auto& child) noexcept -> std::reference_wrapper<const document> {
                    return std::cref(child);
                });
        }
    }
}

struct script_handles {
    QJSValue run{};
    QJSValue log_at{};
    QJSValue count{};
    QJSValue cut{};
};

[[nodiscard]] auto initialize(QJSEngine& engine, const engine_options& options,
                              patch_report& report) -> script_handles {
    engine.globalObject().setProperty(QStringLiteral("globalThis"), engine.globalObject());
    const auto factory = engine.evaluate(qt_text(bootstrap), QStringLiteral("override-host"), 1);
    if (js_failed(engine, factory)) {
        report.message = factory.toString().toStdString();
        fail(override_error::unexpected_failure);
    }
    // UTF-16 units are capped at a quarter of the byte limit. This conservatively
    // bounds UTF-8 output including non-ASCII logs conservatively.
    constexpr auto maximum_utf8_width = 4;
    const auto host = factory.call({
        QJSValue{static_cast<int>(options.max_log_lines)},
        QJSValue{static_cast<int>(options.max_log_bytes / maximum_utf8_width)},
        QJSValue{static_cast<int>(options.max_depth)},
        QJSValue{static_cast<int>(options.max_values)},
    });
    if (js_failed(engine, host)) {
        report.message = host.toString().toStdString();
        fail(override_error::unexpected_failure);
    }
    return {
        .run = host.property(QStringLiteral("run")),
        .log_at = host.property(QStringLiteral("logAt")),
        .count = host.property(QStringLiteral("count")),
        .cut = host.property(QStringLiteral("cut")),
    };
}

auto collect_logs(QJSEngine& engine, const script_handles& host, patch_report& report) -> void {
    const auto count = host.count.call().toInt();
    for (auto index = int{}; index < count; ++index) {
        report.logs.push_back(host.log_at.call({QJSValue{index}}).toString().toStdString());
    }
    report.logs_truncated = host.cut.call().toBool();
    if (engine.hasError()) {
        static_cast<void>(engine.catchError());
        fail(override_error::unexpected_failure);
    }
}

[[nodiscard]] auto evaluate_script(QJSEngine& engine, const script_handles& host,
                                   const document& configuration, const override_item& patch,
                                   patch_report& report) -> QJSValue {
    const auto source =
        std::string{"\"use strict\";\n"} + patch.content +
        "\n; if (typeof main !== 'function') throw new Error('Missing main(config)');"
        "\nmain;\n//# sourceURL=user-override.js\n";
    auto value =
        host.run.call({QJSValue{qt_text(source)}, QJSValue{qt_text(configuration.dump())}});
    if (js_failed(engine, value)) {
        report.error = override_error::script_error;
        report.message = "JavaScript execution failed";
        return {};
    }
    return value;
}

[[nodiscard]] auto budget_expired(const std::stop_token& stop,
                                  std::chrono::steady_clock::time_point deadline) -> bool {
    auto mutex = std::mutex{};
    auto condition = std::condition_variable_any{};
    auto lock = std::unique_lock{mutex};
    static_cast<void>(
        condition.wait_until(lock, stop, deadline, []() noexcept -> bool { return false; }));
    return !stop.stop_requested();
}
} // namespace

auto run_script(const document& configuration, const override_item& patch,
                const engine_options& options, patch_report& report) -> document {
    if (QCoreApplication::instance() == nullptr) {
        fail(override_error::application_required);
    }
    if (!valid_utf8(patch.content)) {
        fail(override_error::invalid_input);
    }
    require_safe_numbers(configuration);
    auto engine = QJSEngine{};
    const auto host = initialize(engine, options, report);
    const auto status = std::make_shared<std::atomic<override_error>>(override_error::none);
    auto watchdog =
        std::jthread{[engine_ref = std::ref(engine), status,
                      deadline = std::chrono::steady_clock::now() +
                                 options.script_timeout](const std::stop_token& stop) -> void {
            // Allocation/system errors must not escape the watcher thread entry.
            const auto waited = guarded<bool>(budget_expired, stop, deadline);
            auto reason = waited.error;
            if (reason == override_error::none && waited.value) {
                reason = override_error::timeout;
            }
            if (reason != override_error::none) {
                status->store(reason);
                engine_ref.get().setInterrupted(true);
            }
        }};
    const auto value = evaluate_script(engine, host, configuration, patch, report);
    static_cast<void>(watchdog.request_stop());
    watchdog.join();
    const auto interrupted = status->load();
    // Only the engine owner resumes execution, after the watchdog has joined.
    engine.setInterrupted(false);
    if (engine.hasError()) {
        static_cast<void>(engine.catchError());
    }
    collect_logs(engine, host, report);
    if (interrupted != override_error::none) {
        fail(interrupted);
    }
    if (report.error != override_error::none) {
        fail(report.error);
    }
    if (!value.property(0).toBool()) {
        report.message = value.property(2).toString().toStdString();
        fail(value.property(1).toString() == QStringLiteral("result")
                 ? override_error::invalid_script_result
                 : override_error::script_error);
    }
    return parse_json(value.property(1).toString().toStdString(), options);
}

} // namespace overrides::detail
// NOLINTEND(llvm-prefer-static-over-anonymous-namespace,altera-struct-pack-align,readability-redundant-member-init,altera-unroll-loops,fuchsia-default-arguments-calls)
