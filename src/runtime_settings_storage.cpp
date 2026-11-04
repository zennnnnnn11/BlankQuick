#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QLockFile>
#include <QtCore/QSaveFile>
#include <QtCore/QString>
#include <QtCore/QUuid>

#include <algorithm>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <limits>
#include <new>
#include <qfiledevice.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "runtime_config.hpp"
#include "runtime_config_detail.hpp"

namespace runtime_config {
namespace {
// CPU file operations have no FPGA unroll requirement.
// NOLINTBEGIN(llvm-prefer-static-over-anonymous-namespace,altera-struct-pack-align,fuchsia-default-arguments-calls,altera-unroll-loops)
constexpr std::size_t revision_size{32};
constexpr int maximum_depth{16};
constexpr std::int64_t maximum_file_size{16777216};
namespace fs = std::filesystem;
using detail::json;

class settings_failure final : public std::exception { // clazy:exclude=copyable-polymorphic
  public:
    explicit settings_failure(settings_error selected) noexcept : error{selected} {}
    [[nodiscard]] auto code() const noexcept -> settings_error {
        return error;
    }

  private:
    settings_error error{};
};
[[noreturn]] auto storage_fail(settings_error error) -> void {
    throw settings_failure{error};
}

[[nodiscard]] auto qt_path(const fs::path& path) -> QString {
    return QString::fromStdWString(path.native());
}

auto safe_path(const fs::path& path) -> void {
    auto current = path.root_path();
    for (const auto& part : path.relative_path()) {
        current /= part;
        const auto info = QFileInfo{qt_path(current)};
        if (info.isSymLink() || info.isJunction()) {
            storage_fail(settings_error::unsafe_path);
        }
    }
}

[[nodiscard]] auto valid_revision(std::string_view revision) noexcept -> bool {
    return revision.size() == revision_size &&
           std::ranges::all_of(revision, [](char value) noexcept -> bool {
               return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
           });
}

[[nodiscard]] auto parse_settings(std::string_view text) -> settings_snapshot {
    auto keys = std::vector<std::unordered_set<std::string>>{};
    const auto document =
        json::parse(text, [&keys](int depth, json::parse_event_t event, json& parsed) -> bool {
            if (depth > maximum_depth) {
                storage_fail(settings_error::corrupt_settings);
            }
            if (event == json::parse_event_t::object_start) {
                keys.emplace_back();
            } else if (event == json::parse_event_t::object_end) {
                keys.pop_back();
            } else if (event == json::parse_event_t::key &&
                       !keys.back().insert(parsed.get<std::string>()).second) {
                storage_fail(settings_error::corrupt_settings);
            }
            return true;
        });
    if (!document.is_object() || document.size() != 3 || !document.contains("version") ||
        !document.at("version").is_number_integer() || document.at("version") != 1 ||
        !document.contains("revision") || !document.at("revision").is_string() ||
        !document.contains("settings")) {
        storage_fail(settings_error::corrupt_settings);
    }
    auto revision = document.at("revision").get<std::string>();
    if (!valid_revision(revision)) {
        storage_fail(settings_error::corrupt_settings);
    }
    try {
        return {
            .settings = detail::settings_from_document(document.at("settings")),
            .revision = std::move(revision),
        };
    } catch (const detail::configuration_failure&) {
        storage_fail(settings_error::corrupt_settings);
    }
}

[[nodiscard]] auto read_snapshot(const settings_storage_options& options) -> settings_snapshot {
    const auto path = options.directory / "runtime-settings.json";
    safe_path(path);
    if (!fs::exists(path)) {
        return {};
    }
    if (!fs::is_regular_file(path)) {
        storage_fail(settings_error::unsafe_path);
    }
    auto file = QFile{qt_path(path)};
    if (!file.open(QIODevice::ReadOnly)) {
        storage_fail(settings_error::io_failure);
    }
    const auto size = file.size();
    if (size < 0 || size > options.max_bytes) {
        storage_fail(settings_error::corrupt_settings);
    }
    const auto bytes = file.read(size + 1);
    if (file.error() != QFileDevice::NoError || bytes.size() != size || !file.atEnd()) {
        storage_fail(settings_error::io_failure);
    }
    return parse_settings(bytes.toStdString());
}

class settings_lock {
  public:
    explicit settings_lock(const settings_storage_options& options)
        : lock{qt_path(options.directory / "runtime-settings.lock")} {
        safe_path(options.directory / "runtime-settings.lock");
        lock.setStaleLockTime(0);
        if (!lock.tryLock(static_cast<int>(options.lock_timeout.count()))) {
            storage_fail(lock.error() == QLockFile::LockFailedError ? settings_error::busy
                                                                    : settings_error::io_failure);
        }
    }

  private:
    QLockFile lock;
};

[[nodiscard]] auto load_impl(const settings_storage_options& options) -> settings_snapshot {
    safe_path(options.directory);
    if (!fs::exists(options.directory)) {
        return {};
    }
    const auto lock = settings_lock{options};
    return read_snapshot(options);
}

[[nodiscard]] auto save_impl(const settings_storage_options& options,
                             const client_settings& settings, std::string_view expected_revision)
    -> settings_snapshot {
    if (!expected_revision.empty() && !valid_revision(expected_revision)) {
        storage_fail(settings_error::invalid_input);
    }
    auto body = detail::settings_document(settings);
    auto candidate = settings_snapshot{
        .settings = settings,
        .revision = QUuid::createUuid().toString(QUuid::Id128).toStdString(),
    };
    const auto bytes =
        json{{"version", 1}, {"revision", candidate.revision}, {"settings", std::move(body)}}
            .dump();
    if (std::ssize(bytes) > options.max_bytes) {
        storage_fail(settings_error::invalid_input);
    }
    safe_path(options.directory);
    static_cast<void>(fs::create_directories(options.directory));
    const auto lock = settings_lock{options};
    const auto previous = read_snapshot(options);
    if (previous.revision != expected_revision) {
        storage_fail(settings_error::conflict);
    }
    const auto path = options.directory / "runtime-settings.json";
    safe_path(path);
    auto file = QSaveFile{qt_path(path)};
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(bytes.data(), std::ssize(bytes)) != std::ssize(bytes) || !file.commit()) {
        storage_fail(settings_error::io_failure);
    }
    return candidate;
}

template <typename Operation, typename... Arguments>
    requires std::invocable<Operation, Arguments...>
[[nodiscard]] auto guarded_settings(bool saving, Operation operation,
                                    Arguments&&... arguments) noexcept -> settings_result {
    try {
        return {.value = std::invoke(operation, std::forward<Arguments>(arguments)...)};
    } catch (const settings_failure& failure) {
        return {.error = failure.code()};
    } catch (const detail::configuration_failure&) {
        return {.error = saving ? settings_error::invalid_input : settings_error::corrupt_settings};
    } catch (const json::exception&) {
        return {.error = settings_error::corrupt_settings};
    } catch (const fs::filesystem_error&) {
        return {.error = settings_error::io_failure};
    } catch (const std::bad_alloc&) {
        return {.error = settings_error::allocation_failure};
    } catch (const std::exception&) {
        return {.error = settings_error::unexpected_failure};
    } catch (...) {
        return {.error = settings_error::unexpected_failure};
    }
}
// NOLINTEND(llvm-prefer-static-over-anonymous-namespace,altera-struct-pack-align,fuchsia-default-arguments-calls,altera-unroll-loops)
} // namespace

settings_manager::settings_manager(settings_storage_options selected)
    : options{std::move(selected)} {
    if (!options.directory.is_absolute() ||
        options.directory != options.directory.lexically_normal() ||
        options.directory == options.directory.root_path() || options.max_bytes < 1 ||
        options.max_bytes > maximum_file_size || options.lock_timeout.count() < 0 ||
        options.lock_timeout.count() > std::numeric_limits<int>::max()) {
        throw std::invalid_argument{"Invalid runtime settings storage options"};
    }
}
auto settings_manager::load() const noexcept -> settings_result {
    return guarded_settings(false, load_impl, options);
}
auto settings_manager::save(const client_settings& settings,
                            std::string_view expected_revision) const noexcept -> settings_result {
    return guarded_settings(true, save_impl, options, settings, expected_revision);
}
} // namespace runtime_config
