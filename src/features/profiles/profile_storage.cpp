#include <QtCore/QByteArray>
#include <QtCore/QFile>
#include <QtCore/QFileDevice>
#include <QtCore/QFileInfo>
#include <QtCore/QLockFile>
#include <QtCore/QSaveFile>
#include <QtCore/QStandardPaths>
#include <QtCore/QString>
#include <QtCore/QUrl>
#include <QtCore/QUuid>

#include <algorithm>
#include <blank/nodes/node_input.hpp>
#include <blank/nodes/unified_node.hpp>
#include <blank/profiles/profile_storage.hpp>
#include <blank/subscriptions/subscription_account.hpp>
#include <blank/subscriptions/subscription_data.hpp>
#include <chrono>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

#include "profile_codec.hpp"
#include "profile_digest.hpp"

namespace profile_storage::detail {
// Per-manager, bounded validation hints. No payloads or global state are retained.
// NOLINTBEGIN(altera-struct-pack-align,readability-redundant-member-init)
namespace {
struct validated_asset {
    std::string id{};
    std::string generation{};
    std::string hash{};
    std::int64_t nodes{};
};
} // namespace
struct validation_state {
    std::mutex mutex{};
    std::vector<validated_asset> assets{};
};
// NOLINTEND(altera-struct-pack-align,readability-redundant-member-init)
} // namespace profile_storage::detail

namespace profile_storage {
namespace {
// The repository uses private namespaces, ISO aggregates and standard spans.
// NOLINTBEGIN(llvm-prefer-static-over-anonymous-namespace,altera-struct-pack-align,readability-redundant-member-init,altera-unroll-loops,fuchsia-default-arguments-calls)
using detail::storage_failure;
namespace fs = std::filesystem;
constexpr std::int64_t identifier_length{32};
constexpr std::int64_t sha256_length{64};
constexpr auto max_port = 65535;
constexpr auto http_ok = 200;
constexpr auto http_not_modified = 304;
constexpr std::int64_t validation_hint_limit{128};

[[nodiscard]] auto same_asset(const detail::validated_asset& asset,
                              const profile_record& record) noexcept -> bool {
    return asset.id == record.id && asset.generation == record.generation &&
           asset.hash == record.cache_sha256 && asset.nodes == record.node_count;
}

[[nodiscard]] auto known_valid(detail::validation_state* state,
                               const profile_record& record) noexcept -> bool {
    if (state == nullptr) {
        return false;
    }
    try {
        const auto lock = std::scoped_lock{state->mutex};
        return std::ranges::any_of(state->assets, [&record](const auto& asset) noexcept -> bool {
            return same_asset(asset, record);
        });
    } catch (...) {
        return false;
    }
}

auto remember_valid(detail::validation_state* state, const profile_record& record) noexcept
    -> void {
    if (state == nullptr) {
        return;
    }
    try {
        auto asset = detail::validated_asset{
            .id = record.id,
            .generation = record.generation,
            .hash = record.cache_sha256,
            .nodes = record.node_count,
        };
        const auto lock = std::scoped_lock{state->mutex};
        const auto previous =
            std::ranges::find(state->assets, record.id, &detail::validated_asset::id);
        if (previous != state->assets.end()) {
            *previous = std::move(asset);
            return;
        }
        if (std::ssize(state->assets) >= validation_hint_limit) {
            state->assets.erase(state->assets.begin());
        }
        state->assets.push_back(std::move(asset));
    } catch (...) {
        // An optional acceleration hint must never turn a published success into failure.
        return;
    }
}

[[noreturn]] auto fail(storage_error error) -> void {
    throw storage_failure{error};
}

template <typename Value, typename Operation, typename... Arguments>
    requires std::invocable<Operation, Arguments...>
[[nodiscard]] auto guarded(Operation&& operation, Arguments&&... arguments) noexcept
    -> storage_result<Value> {
    try {
        return {
            .value = std::invoke(std::forward<Operation>(operation),
                                 std::forward<Arguments>(arguments)...),
        };
    } catch (const storage_failure& failure) {
        return {.error = failure.code()};
    } catch (const std::bad_alloc&) {
        return {.error = storage_error::allocation_failure};
    } catch (const fs::filesystem_error&) {
        return {.error = storage_error::io_failure};
    } catch (const std::exception&) {
        return {.error = storage_error::unexpected_failure};
    } catch (...) {
        return {.error = storage_error::unexpected_failure};
    }
}

[[nodiscard]] auto qt_path(const fs::path& path) -> QString {
    return QString::fromStdWString(path.native());
}

[[nodiscard]] auto is_hex(std::string_view text, std::int64_t length) noexcept -> bool {
    return std::ssize(text) == length &&
           std::ranges::all_of(text, [](char character) noexcept -> bool {
               return (character >= '0' && character <= '9') ||
                      (character >= 'a' && character <= 'f');
           });
}

auto require_id(std::string_view profile_id) -> void {
    if (!is_hex(profile_id, identifier_length)) {
        fail(storage_error::invalid_input);
    }
}

[[nodiscard]] auto new_id() -> std::string {
    return QUuid::createUuid().toString(QUuid::Id128).toStdString();
}

[[nodiscard]] auto now_seconds() noexcept -> std::int64_t {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

[[nodiscard]] auto reparse_point(const fs::path& path) -> bool {
    const auto information = QFileInfo{qt_path(path)};
    return information.isSymLink() || information.isJunction();
}

auto safe_path(const fs::path& root, const fs::path& path) -> void {
    const auto relative = path.lexically_relative(root);
    if (relative.empty() && path != root) {
        fail(storage_error::unsafe_path);
    }
    auto current = root;
    if (reparse_point(current)) {
        fail(storage_error::unsafe_path);
    }
    for (const auto& part : relative) {
        if (part == fs::path{".."}) {
            fail(storage_error::unsafe_path);
        }
        current /= part;
        if (reparse_point(current)) {
            fail(storage_error::unsafe_path);
        }
    }
}

auto make_directory(const fs::path& root, const fs::path& path) -> void {
    safe_path(root, path);
    static_cast<void>(fs::create_directories(path));
    if (!fs::is_directory(path)) {
        fail(storage_error::io_failure);
    }
}

[[nodiscard]] auto read_file(const fs::path& root, const fs::path& path, std::int64_t limit)
    -> std::string {
    safe_path(root, path);
    if (!fs::is_regular_file(path)) {
        fail(storage_error::io_failure);
    }
    auto file = QFile{qt_path(path)};
    if (!file.open(QIODevice::ReadOnly)) {
        fail(storage_error::io_failure);
    }
    const auto expected = file.size();
    if (expected < 0) {
        fail(storage_error::io_failure);
    }
    if (expected > limit) {
        fail(storage_error::resource_limit);
    }
    const auto data = file.read(expected + 1);
    if (file.error() != QFileDevice::NoError) {
        fail(storage_error::io_failure);
    }
    if (data.size() > limit) {
        fail(storage_error::resource_limit);
    }
    if (data.size() != expected || !file.atEnd()) {
        fail(storage_error::io_failure);
    }
    return data.toStdString();
}

auto write_file(const fs::path& root, const fs::path& path, std::string_view data) -> void {
    safe_path(root, path);
    if (fs::exists(path) && !fs::is_regular_file(path)) {
        fail(storage_error::unsafe_path);
    }
    auto file = QSaveFile{qt_path(path)};
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(std::to_address(data.begin()), std::ssize(data)) != std::ssize(data) ||
        !file.commit()) {
        fail(storage_error::io_failure);
    }
}

[[nodiscard]] auto digest(std::string_view data) -> std::string {
    return detail::sha256(data);
}

auto check_size(std::int64_t size, std::int64_t limit) -> void {
    if (size < 0 || size > limit) {
        fail(storage_error::resource_limit);
    }
}

auto validate_settings(const profile_settings& settings) -> void {
    constexpr auto name_limit = 4096;
    if (settings.name.empty() || settings.name.size() > name_limit ||
        std::ranges::find(settings.name, '\0') != settings.name.end() ||
        (settings.auto_update_interval && settings.auto_update_interval->count() <= 0)) {
        fail(storage_error::invalid_input);
    }
    if (settings.subscription_url.empty()) {
        return;
    }
    constexpr auto url_limit = 8192;
    if (std::ssize(settings.subscription_url) > url_limit ||
        settings.subscription_url.find_first_of("\r\n") != std::string::npos ||
        std::ranges::find(settings.subscription_url, '\0') != settings.subscription_url.end()) {
        fail(storage_error::invalid_input);
    }
    const auto url = QUrl::fromEncoded(
        QByteArray{settings.subscription_url.data(), std::ssize(settings.subscription_url)},
        QUrl::StrictMode);
    if (!url.isValid() || url.host().isEmpty() ||
        (url.scheme() != QStringLiteral("http") && url.scheme() != QStringLiteral("https"))) {
        fail(storage_error::invalid_input);
    }
}

auto validate_account(const subscription_data::account_metadata& account) -> void {
    for (const auto& value : {
             account.upload_bytes,
             account.download_bytes,
             account.total_bytes,
             account.expire_unix_seconds,
         }) {
        if (value && *value < 0) {
            fail(storage_error::invalid_input);
        }
    }
    if (account.update_interval && account.update_interval->count() <= 0) {
        fail(storage_error::invalid_input);
    }
}

auto validate_record(const profile_record& record) -> void {
    if (!is_hex(record.id, identifier_length) || !is_hex(record.revision, identifier_length) ||
        !is_hex(record.generation, identifier_length) ||
        !is_hex(record.raw_sha256, sha256_length) || !is_hex(record.cache_sha256, sha256_length) ||
        record.created_unix_seconds < 0 ||
        record.updated_unix_seconds < record.created_unix_seconds ||
        record.checked_unix_seconds < record.updated_unix_seconds || record.node_count <= 0 ||
        record.raw_bytes <= 0 || record.cache_bytes <= 0) {
        fail(storage_error::corrupt_index);
    }
    try {
        validate_settings(record.settings);
        validate_account(record.account);
    } catch (const storage_failure&) {
        fail(storage_error::corrupt_index);
    }
}

auto validate_nodes(std::span<const node_model::unified_node> nodes, storage_error error) -> void {
    if (nodes.empty()) {
        fail(error);
    }
    for (const auto& node : nodes) {
        const auto& metadata = node.metadata;
        if (node.core.protocol == node_model::protocol_type::unspecified ||
            !node_model::has_matching_protocol(node) || node.core.name.empty() ||
            !std::isfinite(metadata.multiplier) || metadata.multiplier < 0 ||
            (metadata.latency && metadata.latency->count() < 0) ||
            (metadata.download_bytes_per_second && *metadata.download_bytes_per_second < 0)) {
            fail(error);
        }
        if (node.core.remote && (node.core.remote->host.empty() || node.core.remote->port <= 0 ||
                                 node.core.remote->port > max_port)) {
            fail(error);
        }
    }
}

struct prepared_assets {
    std::string cache{};
    std::string raw_hash{};
    std::string cache_hash{};
    std::int64_t nodes{};
};

[[nodiscard]] auto prepare_assets(const storage_options& options, std::string_view raw,
                                  std::span<const node_model::unified_node> nodes)
    -> prepared_assets {
    check_size(std::ssize(raw), options.max_raw_bytes);
    check_size(std::ssize(nodes), options.max_nodes);
    if (raw.empty()) {
        fail(storage_error::invalid_input);
    }
    validate_nodes(nodes, storage_error::invalid_input);
    auto cache = detail::encode_nodes(nodes);
    check_size(std::ssize(cache), options.max_cache_bytes);
    auto raw_hash = digest(raw);
    auto cache_hash = digest(cache);
    return {
        .cache = std::move(cache),
        .raw_hash = std::move(raw_hash),
        .cache_hash = std::move(cache_hash),
        .nodes = std::ssize(nodes),
    };
}

auto validate_raw_bytes(const profile_record& record, std::string_view data) -> void {
    if (std::ssize(data) != record.raw_bytes || digest(data) != record.raw_sha256) {
        fail(storage_error::corrupt_raw);
    }
}

auto validate_cache_bytes(const profile_record& record, std::string_view data) -> void {
    if (std::ssize(data) != record.cache_bytes || digest(data) != record.cache_sha256) {
        fail(storage_error::corrupt_cache);
    }
}

[[nodiscard]] auto decode_cached_nodes(const profile_record& record, std::string_view data,
                                       std::int64_t limit, detail::validation_state* state)
    -> std::vector<node_model::unified_node> {
    validate_cache_bytes(record, data);
    auto nodes = detail::decode_nodes(data, limit);
    if (std::ssize(nodes) != record.node_count) {
        fail(storage_error::corrupt_cache);
    }
    validate_nodes(nodes, storage_error::corrupt_cache);
    remember_valid(state, record);
    return nodes;
}

auto ensure_cached_nodes(const profile_record& record, std::string_view data, std::int64_t limit,
                         detail::validation_state* state) -> void {
    validate_cache_bytes(record, data);
    if (!known_valid(state, record)) {
        auto nodes = detail::decode_nodes(data, limit);
        if (std::ssize(nodes) != record.node_count) {
            fail(storage_error::corrupt_cache);
        }
        validate_nodes(nodes, storage_error::corrupt_cache);
        remember_valid(state, record);
    }
}

class store_session {
  public:
    explicit store_session(const storage_options& selected)
        : options{selected},
          lock_file{std::make_unique<QLockFile>(qt_path(selected.directory / "profiles.lock"))} {
        make_directory(options.directory, options.directory);
        safe_path(options.directory, options.directory / "profiles.lock");
        lock_file->setStaleLockTime(std::chrono::milliseconds::zero());
        if (!lock_file->tryLock(options.lock_timeout)) {
            fail(lock_file->error() == QLockFile::LockFailedError ? storage_error::busy
                                                                  : storage_error::io_failure);
        }
        safe_path(options.directory, options.directory / "profiles");
    }

    [[nodiscard]] auto index() const -> std::vector<profile_record> {
        const auto path = options.directory / "profiles.json";
        safe_path(options.directory, path);
        if (!fs::exists(path)) {
            const auto profiles = options.directory / "profiles";
            if (fs::exists(profiles) &&
                fs::directory_iterator{profiles} != fs::directory_iterator{}) {
                fail(storage_error::corrupt_index);
            }
            return {};
        }
        auto records = detail::decode_index(
            read_file(options.directory, path, options.max_index_bytes), options.max_profiles);
        auto seen = std::unordered_set<std::string>{};
        for (const auto& record : records) {
            validate_record(record);
            if (!seen.insert(record.id).second) {
                fail(storage_error::corrupt_index);
            }
        }
        return records;
    }

    auto commit(std::span<const profile_record> records) const -> void {
        check_size(std::ssize(records), options.max_profiles);
        const auto text = detail::encode_index(records);
        check_size(std::ssize(text), options.max_index_bytes);
        write_file(options.directory, options.directory / "profiles.json", text);
    }

    [[nodiscard]] auto generation_path(const profile_record& record) const -> fs::path {
        return options.directory / "profiles" / record.id / record.generation;
    }

    auto stage(profile_record& record, std::string_view raw, const prepared_assets& prepared) const
        -> void {
        // 先写不可变代次；profiles.json 提交才是对读者可见的发布点。
        record.generation = new_id();
        record.node_count = prepared.nodes;
        record.raw_bytes = std::ssize(raw);
        record.cache_bytes = std::ssize(prepared.cache);
        record.raw_sha256 = prepared.raw_hash;
        record.cache_sha256 = prepared.cache_hash;
        const auto path = generation_path(record);
        safe_path(options.directory, path);
        if (fs::exists(path)) {
            fail(storage_error::conflict);
        }
        make_directory(options.directory, path);
        write_file(options.directory, path / "raw.txt", raw);
        write_file(options.directory, path / "nodes.json", prepared.cache);
    }

    [[nodiscard]] auto read_raw_bytes(const profile_record& record) const -> std::string {
        check_size(record.raw_bytes, options.max_raw_bytes);
        auto data = read_file(options.directory, generation_path(record) / "raw.txt",
                              options.max_raw_bytes);
        return data;
    }

    [[nodiscard]] auto read_cache_bytes(const profile_record& record) const -> std::string {
        check_size(record.cache_bytes, options.max_cache_bytes);
        check_size(record.node_count, options.max_nodes);
        return read_file(options.directory, generation_path(record) / "nodes.json",
                         options.max_cache_bytes);
    }

  private:
    storage_options options;
    std::unique_ptr<QLockFile> lock_file;
};

[[nodiscard]] auto find_record(std::vector<profile_record>& records, std::string_view profile_id)
    -> profile_record& {
    require_id(profile_id);
    const auto found = std::ranges::find(records, profile_id, &profile_record::id);
    if (found == records.end()) {
        fail(storage_error::not_found);
    }
    return *found;
}

auto match_revision(const profile_record& record, std::string_view revision) -> void {
    require_id(revision);
    if (record.revision != revision) {
        fail(storage_error::conflict);
    }
}

enum class asset_selection : std::int8_t { nodes, raw, both };
struct asset_request {
    std::string_view id{};
    std::optional<std::string_view> revision{};
    asset_selection selection{asset_selection::both};
};
struct encoded_snapshot {
    profile_record profile{};
    std::string cache{};
    std::string raw{};
};

[[nodiscard]] auto capture_assets(const storage_options& options, const asset_request& request)
    -> encoded_snapshot {
    // 原文与缓存在同一锁内读取，随后在锁外解码，避免混合不同代次。
    const auto session = store_session{options};
    auto records = session.index();
    const auto& record = find_record(records, request.id);
    if (request.revision) {
        match_revision(record, *request.revision);
    }
    auto result = encoded_snapshot{.profile = record};
    if (request.selection != asset_selection::raw) {
        result.cache = session.read_cache_bytes(record);
    }
    if (request.selection != asset_selection::nodes) {
        result.raw = session.read_raw_bytes(record);
    }
    return result;
}

template <typename Value>
auto patch(std::optional<Value>& target, const std::optional<Value>& source) noexcept(
    std::is_nothrow_copy_assignable_v<std::optional<Value>>) -> void {
    if (source) {
        target = source;
    }
}

auto merge_account(subscription_data::account_metadata& target,
                   const subscription_data::account_metadata& source) -> void {
    validate_account(source);
    patch(target.upload_bytes, source.upload_bytes);
    patch(target.download_bytes, source.download_bytes);
    patch(target.total_bytes, source.total_bytes);
    patch(target.expire_unix_seconds, source.expire_unix_seconds);
    patch(target.profile_name, source.profile_name);
    patch(target.suggested_filename, source.suggested_filename);
    patch(target.home_page_url, source.home_page_url);
    patch(target.plan_name, source.plan_name);
    patch(target.update_interval, source.update_interval);
    patch(target.etag, source.etag);
    patch(target.last_modified, source.last_modified);
}

auto touch(profile_record& record, bool body_replaced) -> void {
    record.revision = new_id();
    record.checked_unix_seconds = std::max(record.checked_unix_seconds, now_seconds());
    if (body_replaced) {
        record.updated_unix_seconds = record.checked_unix_seconds;
    }
}

[[nodiscard]] auto remove_tree(const fs::path& root, const fs::path& target) noexcept -> bool {
    try {
        safe_path(root, target);
        if (!fs::exists(target)) {
            return true;
        }
        if (!fs::is_directory(target) || target == root) {
            return false;
        }
        // Inspect every descendant before remove_all; never traverse a junction
        // or symbolic link. The lock excludes cooperating filesystem mutations.
        const auto entries = fs::recursive_directory_iterator{target};
        const auto accepted =
            std::ranges::all_of(entries, [&root](const fs::directory_entry& entry) -> bool {
                safe_path(root, entry.path());
                const auto information = QFileInfo{qt_path(entry.path())};
                return information.isDir() || information.isFile();
            });
        if (!accepted) {
            return false;
        }
        static_cast<void>(fs::remove_all(target));
        return true;
    } catch (const storage_failure&) {
        return false;
    } catch (const fs::filesystem_error&) {
        return false;
    } catch (...) {
        return false;
    }
}

[[nodiscard]] auto accepted_fetch(const subscription_data::fetch_result& fetched) noexcept -> bool {
    return fetched.error == subscription_data::fetch_error::none &&
           fetched.subscription.error == node_input::parse_error::none &&
           ((fetched.not_modified && fetched.http_status == http_not_modified &&
             fetched.raw_body.empty() && fetched.subscription.nodes.empty()) ||
            (!fetched.not_modified && fetched.http_status == http_ok && !fetched.raw_body.empty() &&
             !fetched.subscription.nodes.empty()));
}

auto record_cleanup(cleanup_result& result, bool removed) noexcept -> void {
    result.cleanup_pending = result.cleanup_pending || !removed;
    result.removed_directories += removed ? 1 : 0;
}

[[nodiscard]] auto clean_generations(const store_session& session, const storage_options& settings,
                                     const profile_record& active, detail::validation_state* state)
    -> cleanup_result {
    const auto directory = session.generation_path(active).parent_path();
    auto obsolete = std::vector<fs::path>{};
    auto result = cleanup_result{};
    for (const auto& generation : fs::directory_iterator{directory}) {
        const auto name = generation.path().filename().string();
        if (is_hex(name, identifier_length) && name != active.generation) {
            obsolete.push_back(generation.path());
        }
    }
    if (obsolete.empty()) {
        return result;
    }
    // Preserve recoverable backups if the currently published assets are bad.
    validate_raw_bytes(active, session.read_raw_bytes(active));
    ensure_cached_nodes(active, session.read_cache_bytes(active), settings.max_nodes, state);
    for (const auto& path : obsolete) {
        record_cleanup(result, remove_tree(settings.directory, path));
    }
    return result;
}

// Allocate the public return value before publishing, so an allocation failure
// cannot be reported after the index has already committed.
[[nodiscard]] auto prepare_record(const profile_record& record) -> profile_record {
    return record;
}

[[nodiscard]] auto store_list(const storage_options& options) -> std::vector<profile_record>;
[[nodiscard]] auto store_get(const storage_options& options, std::string_view profile_id)
    -> profile_record;
[[nodiscard]] auto store_load_nodes(const storage_options& options, detail::validation_state* state,
                                    std::string_view profile_id) -> node_snapshot;
[[nodiscard]] auto store_load_raw(const storage_options& options, std::string_view profile_id)
    -> raw_snapshot;
[[nodiscard]] auto store_load_material(const storage_options& options,
                                       detail::validation_state* state, std::string_view profile_id)
    -> material_snapshot;
[[nodiscard]] auto store_save_new(const storage_options& options, const profile_input& input)
    -> profile_record;
[[nodiscard]] auto store_save_fetched(const storage_options& options, profile_settings settings,
                                      const subscription_data::fetch_result& fetched)
    -> profile_record;
[[nodiscard]] auto store_replace(const storage_options& options, const profile_refresh& input)
    -> profile_record;
[[nodiscard]] auto store_edit(const storage_options& options, const profile_edit& input)
    -> profile_record;
[[nodiscard]] auto store_update_metadata(const storage_options& options,
                                         const metadata_update& input) -> profile_record;
[[nodiscard]] auto store_apply_fetch(const storage_options& options,
                                     detail::validation_state* state, std::string_view profile_id,
                                     std::string_view expected_revision,
                                     const subscription_data::fetch_result& fetched)
    -> profile_record;
[[nodiscard]] auto store_remove(const storage_options& options, std::string_view profile_id,
                                std::string_view expected_revision) -> cleanup_result;
[[nodiscard]] auto store_collect_garbage(const storage_options& options,
                                         detail::validation_state* state) -> cleanup_result;

[[nodiscard]] auto store_list(const storage_options& options) -> std::vector<profile_record> {
    const auto session = store_session{options};
    return session.index();
}

[[nodiscard]] auto store_get(const storage_options& options, std::string_view profile_id)
    -> profile_record {
    const auto session = store_session{options};
    auto records = session.index();
    return find_record(records, profile_id);
}

[[nodiscard]] auto store_load_nodes(const storage_options& options, detail::validation_state* state,
                                    std::string_view profile_id) -> node_snapshot {
    auto captured =
        capture_assets(options, {.id = profile_id, .selection = asset_selection::nodes});
    auto nodes = decode_cached_nodes(captured.profile, captured.cache, options.max_nodes, state);
    return {.profile = std::move(captured.profile), .nodes = std::move(nodes)};
}

[[nodiscard]] auto store_load_raw(const storage_options& options, std::string_view profile_id)
    -> raw_snapshot {
    auto captured = capture_assets(options, {.id = profile_id, .selection = asset_selection::raw});
    validate_raw_bytes(captured.profile, captured.raw);
    return {.profile = std::move(captured.profile), .raw = std::move(captured.raw)};
}

[[nodiscard]] auto store_load_material(const storage_options& options,
                                       detail::validation_state* state, std::string_view profile_id)
    -> material_snapshot {
    auto captured = capture_assets(options, {.id = profile_id});
    auto nodes = decode_cached_nodes(captured.profile, captured.cache, options.max_nodes, state);
    validate_raw_bytes(captured.profile, captured.raw);
    return {
        .profile = std::move(captured.profile),
        .nodes = std::move(nodes),
        .raw = std::move(captured.raw),
    };
}

[[nodiscard]] auto store_save_new(const storage_options& options, const profile_input& input)
    -> profile_record {
    validate_settings(input.settings);
    validate_account(input.account);
    const auto prepared = prepare_assets(options, input.raw, input.nodes);
    const auto session = store_session{options};
    auto records = session.index();
    check_size(std::ssize(records) + 1, options.max_profiles);
    // Commit an empty index before staging the first generation, so a crash
    // or failed first publication leaves a recognizable, recoverable store.
    if (!fs::exists(options.directory / "profiles.json")) {
        session.commit(records);
    }
    auto record = profile_record{
        .id = new_id(),
        .revision = new_id(),
        .settings = input.settings,
        .account = input.account,
    };
    if (fs::exists(options.directory / "profiles" / record.id)) {
        fail(storage_error::conflict);
    }
    record.created_unix_seconds = now_seconds();
    touch(record, true);
    session.stage(record, input.raw, prepared);
    records.push_back(record);
    session.commit(records);
    return record;
}

[[nodiscard]] auto store_save_fetched(const storage_options& options, profile_settings settings,
                                      const subscription_data::fetch_result& fetched)
    -> profile_record {
    if (!accepted_fetch(fetched) || fetched.not_modified) {
        fail(storage_error::fetch_failed);
    }
    if (settings.name.empty()) {
        settings.name = fetched.metadata.account.profile_name.value_or("订阅");
    }
    return store_save_new(options, {
                                       .settings = std::move(settings),
                                       .account = fetched.metadata.account,
                                       .raw = fetched.raw_body,
                                       .nodes = fetched.subscription.nodes,
                                   });
}

[[nodiscard]] auto store_replace(const storage_options& options, const profile_refresh& input)
    -> profile_record {
    // Preserve conflict precedence, then recheck revision after CPU preparation.
    const auto current = store_get(options, input.id);
    match_revision(current, input.expected_revision);
    validate_account(input.account);
    const auto prepared = prepare_assets(options, input.raw, input.nodes);
    const auto session = store_session{options};
    auto records = session.index();
    auto& record = find_record(records, input.id);
    match_revision(record, input.expected_revision);
    merge_account(record.account, input.account);
    // A fresh body without validators invalidates old conditional requests.
    record.account.etag = input.account.etag;
    record.account.last_modified = input.account.last_modified;
    touch(record, true);
    session.stage(record, input.raw, prepared);
    auto published = prepare_record(record);
    session.commit(records);
    return published;
}

[[nodiscard]] auto store_edit(const storage_options& options, const profile_edit& input)
    -> profile_record {
    validate_settings(input.settings);
    const auto session = store_session{options};
    auto records = session.index();
    auto& record = find_record(records, input.id);
    match_revision(record, input.expected_revision);
    if (record.settings.subscription_url != input.settings.subscription_url) {
        record.account.etag.reset();
        record.account.last_modified.reset();
    }
    record.settings = input.settings;
    record.revision = new_id();
    auto published = prepare_record(record);
    session.commit(records);
    return published;
}

[[nodiscard]] auto store_update_metadata(const storage_options& options,
                                         const metadata_update& input) -> profile_record {
    const auto session = store_session{options};
    auto records = session.index();
    auto& record = find_record(records, input.id);
    match_revision(record, input.expected_revision);
    merge_account(record.account, input.account);
    touch(record, false);
    auto published = prepare_record(record);
    session.commit(records);
    return published;
}

[[nodiscard]] auto store_apply_fetch(const storage_options& options,
                                     detail::validation_state* state, std::string_view profile_id,
                                     std::string_view expected_revision,
                                     const subscription_data::fetch_result& fetched)
    -> profile_record {
    if (!accepted_fetch(fetched)) {
        fail(storage_error::fetch_failed);
    }
    if (fetched.not_modified) {
        const auto captured =
            capture_assets(options, {.id = profile_id, .revision = expected_revision});
        validate_raw_bytes(captured.profile, captured.raw);
        ensure_cached_nodes(captured.profile, captured.cache, options.max_nodes, state);
        const auto session = store_session{options};
        auto records = session.index();
        auto& record = find_record(records, profile_id);
        match_revision(record, expected_revision);
        // Captured bytes were checked outside the file lock; the revision check
        // above prevents publishing over a concurrent refresh, edit or deletion.
        merge_account(record.account, fetched.metadata.account);
        touch(record, false);
        auto published = prepare_record(record);
        session.commit(records);
        return published;
    }
    return store_replace(options, {
                                      .id = std::string{profile_id},
                                      .expected_revision = std::string{expected_revision},
                                      .account = fetched.metadata.account,
                                      .raw = fetched.raw_body,
                                      .nodes = fetched.subscription.nodes,
                                  });
}

[[nodiscard]] auto store_remove(const storage_options& options, std::string_view profile_id,
                                std::string_view expected_revision) -> cleanup_result {
    const auto session = store_session{options};
    auto records = session.index();
    match_revision(find_record(records, profile_id), expected_revision);
    std::erase_if(records, [profile_id](const profile_record& record) noexcept -> bool {
        return record.id == profile_id;
    });
    const auto path = options.directory / "profiles" / std::string{profile_id};
    session.commit(records);
    const auto removed = remove_tree(options.directory, path);
    return {.cleanup_pending = !removed, .removed_directories = removed ? 1 : 0};
}

[[nodiscard]] auto store_collect_garbage(const storage_options& options,
                                         detail::validation_state* state) -> cleanup_result {
    const auto session = store_session{options};
    const auto records = session.index();
    const auto parent = options.directory / "profiles";
    if (!fs::exists(parent)) {
        return {};
    }
    auto result = cleanup_result{};
    for (const auto& directory : fs::directory_iterator{parent}) {
        const auto profile_id = directory.path().filename().string();
        if (!is_hex(profile_id, identifier_length)) {
            continue;
        }
        safe_path(options.directory, directory.path());
        const auto found = std::ranges::find(records, profile_id, &profile_record::id);
        if (found == records.end()) {
            record_cleanup(result, remove_tree(options.directory, directory.path()));
            continue;
        }
        const auto cleaned = clean_generations(session, options, *found, state);
        result.cleanup_pending = result.cleanup_pending || cleaned.cleanup_pending;
        result.removed_directories += cleaned.removed_directories;
    }
    return result;
}

[[nodiscard]] auto checked_options(storage_options selected) -> storage_options {
    if (selected.directory.empty() || !selected.directory.is_absolute() ||
        selected.lock_timeout.count() < 0 || selected.lock_timeout > std::chrono::minutes{1}) {
        throw std::invalid_argument{"Invalid profile storage options"};
    }
    for (const auto limit : {
             selected.max_profiles,
             selected.max_index_bytes,
             selected.max_raw_bytes,
             selected.max_cache_bytes,
             selected.max_nodes,
         }) {
        if (limit <= 0 || limit == std::numeric_limits<std::int64_t>::max()) {
            throw std::invalid_argument{"Invalid profile storage limit"};
        }
    }
    selected.directory = selected.directory.lexically_normal();
    return selected;
}

} // namespace

auto default_data_directory() -> fs::path {
    return fs::path{
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation).toStdWString()};
}

auto used_bytes(const profile_record& profile) noexcept -> std::optional<std::int64_t> {
    const auto& account = profile.account;
    if (!account.upload_bytes || !account.download_bytes || *account.upload_bytes < 0 ||
        *account.download_bytes < 0 ||
        *account.upload_bytes >
            std::numeric_limits<std::int64_t>::max() - *account.download_bytes) {
        return std::nullopt;
    }
    return *account.upload_bytes + *account.download_bytes;
}

profile_manager::profile_manager(storage_options selected)
    : options{checked_options(std::move(selected))},
      validation{std::make_shared<detail::validation_state>()} {}

auto profile_manager::list() const noexcept -> storage_result<std::vector<profile_record>> {
    return guarded<std::vector<profile_record>>(store_list, options);
}

auto profile_manager::get(std::string_view profile_id) const noexcept
    -> storage_result<profile_record> {
    return guarded<profile_record>(store_get, options, profile_id);
}

auto profile_manager::load_nodes(std::string_view profile_id) const noexcept
    -> storage_result<node_snapshot> {
    return guarded<node_snapshot>(store_load_nodes, options, validation.get(), profile_id);
}

auto profile_manager::load_raw(std::string_view profile_id) const noexcept
    -> storage_result<raw_snapshot> {
    return guarded<raw_snapshot>(store_load_raw, options, profile_id);
}

auto profile_manager::load_material(std::string_view profile_id) const noexcept
    -> storage_result<material_snapshot> {
    return guarded<material_snapshot>(store_load_material, options, validation.get(), profile_id);
}

auto profile_manager::save_new(const profile_input& input) const noexcept
    -> storage_result<profile_record> {
    auto result = guarded<profile_record>(store_save_new, options, input);
    if (result.error == storage_error::none) {
        remember_valid(validation.get(), result.value);
    }
    return result;
}

auto profile_manager::save_fetched(profile_settings settings,
                                   const subscription_data::fetch_result& fetched) const noexcept
    -> storage_result<profile_record> {
    auto result =
        guarded<profile_record>(store_save_fetched, options, std::move(settings), fetched);
    if (result.error == storage_error::none) {
        remember_valid(validation.get(), result.value);
    }
    return result;
}

auto profile_manager::replace(const profile_refresh& input) const noexcept
    -> storage_result<profile_record> {
    auto result = guarded<profile_record>(store_replace, options, input);
    if (result.error == storage_error::none) {
        remember_valid(validation.get(), result.value);
    }
    return result;
}

auto profile_manager::edit(const profile_edit& input) const noexcept
    -> storage_result<profile_record> {
    return guarded<profile_record>(store_edit, options, input);
}

auto profile_manager::update_metadata(const metadata_update& input) const noexcept
    -> storage_result<profile_record> {
    return guarded<profile_record>(store_update_metadata, options, input);
}

auto profile_manager::apply_fetch(std::string_view profile_id, std::string_view expected_revision,
                                  const subscription_data::fetch_result& fetched) const noexcept
    -> storage_result<profile_record> {
    auto result = guarded<profile_record>(store_apply_fetch, options, validation.get(), profile_id,
                                          expected_revision, fetched);
    if (result.error == storage_error::none) {
        remember_valid(validation.get(), result.value);
    }
    return result;
}

auto profile_manager::remove(std::string_view profile_id,
                             std::string_view expected_revision) const noexcept
    -> storage_result<cleanup_result> {
    return guarded<cleanup_result>(store_remove, options, profile_id, expected_revision);
}

auto profile_manager::collect_garbage() const noexcept -> storage_result<cleanup_result> {
    return guarded<cleanup_result>(store_collect_garbage, options, validation.get());
}

} // namespace profile_storage

// NOLINTEND(llvm-prefer-static-over-anonymous-namespace,altera-struct-pack-align,readability-redundant-member-init,altera-unroll-loops,fuchsia-default-arguments-calls)
