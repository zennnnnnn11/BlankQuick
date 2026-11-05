#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QLockFile>
#include <QtCore/QSaveFile>
#include <QtCore/QString>
#include <QtCore/QUuid>

#include <algorithm>
#include <blank/runtime_config/runtime_config.hpp>
#include <blank/runtime_config/settings_codec.hpp>
#include <blank/runtime_config/settings_storage.hpp>
#include <blank/shared/json_document.hpp>
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

namespace runtime_config {
namespace {

// NOLINTBEGIN(llvm-prefer-static-over-anonymous-namespace,altera-struct-pack-align,fuchsia-default-arguments-calls,altera-unroll-loops)
constexpr std::size_t revision_size{32};
constexpr int maximum_depth{16};
constexpr std::int64_t maximum_file_size{16777216};
namespace fs = std::filesystem;
using json = json_detail::document;

class settings_failure final : public std::exception {
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

[[nodiscard]] constexpr auto codec_error(configuration_error error,
                                         settings_error rejected) noexcept -> settings_error {
    if (error == configuration_error::allocation_failure) {
        return settings_error::allocation_failure;
    }
    if (error == configuration_error::none || error == configuration_error::unexpected_failure) {
        return settings_error::unexpected_failure;
    }
    return rejected;
}

static_assert(codec_error(configuration_error::allocation_failure,
                          settings_error::corrupt_settings) == settings_error::allocation_failure);
static_assert(codec_error(configuration_error::invalid_settings, settings_error::invalid_input) ==
              settings_error::invalid_input);

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
    auto decoded = decode_settings(document.at("settings"));
    if (decoded.error != configuration_error::none || !decoded.value.has_value()) {
        storage_fail(codec_error(decoded.error, settings_error::corrupt_settings));
    }
    return {.settings = std::move(*decoded.value), .revision = std::move(revision)};
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
    // 先在锁外验证和编码候选，缩短占锁时间；取得锁后必须重新读取现场版本。
    // 版本比较与 QSaveFile 提交在同一临界区，构成一次不可分割的乐观并发更新。
    if (!expected_revision.empty() && !valid_revision(expected_revision)) {
        storage_fail(settings_error::invalid_input);
    }
    auto encoded = encode_settings(settings);
    if (encoded.error != configuration_error::none || !encoded.value.has_value()) {
        storage_fail(codec_error(encoded.error, settings_error::invalid_input));
    }
    auto body = std::move(*encoded.value);
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
    // 禁止退回直接覆盖目标文件；原子替换不可用时宁可失败，也不留下半份设置。
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(bytes.data(), std::ssize(bytes)) != std::ssize(bytes) || !file.commit()) {
        storage_fail(settings_error::io_failure);
    }
    return candidate;
}

template <typename Operation, typename... Arguments>
    requires std::invocable<Operation, Arguments...>
[[nodiscard]] auto guarded_settings(Operation operation, Arguments&&... arguments) noexcept
    -> settings_result {
    try {
        return {.value = std::invoke(operation, std::forward<Arguments>(arguments)...)};
    } catch (const settings_failure& failure) {
        return {.error = failure.code()};

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
    return guarded_settings(load_impl, options);
}
auto settings_manager::save(const client_settings& settings,
                            std::string_view expected_revision) const noexcept -> settings_result {
    return guarded_settings(save_impl, options, settings, expected_revision);
}
} // namespace runtime_config
