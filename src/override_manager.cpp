#include "override_manager.hpp"

#include <QtCore/QByteArrayView>
#include <QtCore/QCryptographicHash>
#include <QtCore/QFile>
#include <QtCore/QFileDevice>
#include <QtCore/QFileInfo>
#include <QtCore/QLockFile>
#include <QtCore/QSaveFile>
#include <QtCore/QString>
#include <QtCore/QUuid>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

#include "json_document.hpp"
#include "override_detail.hpp"
#include "override_engine.hpp"
#include "override_types.hpp"

namespace overrides {
namespace {
// Same ISO CPU/storage conventions as profile_storage; no runtime policy is shared.
// NOLINTBEGIN(llvm-prefer-static-over-anonymous-namespace,altera-struct-pack-align,readability-redundant-member-init,altera-unroll-loops,fuchsia-default-arguments-calls)
namespace fs = std::filesystem;
using detail::fail;
using json_detail::document;

struct index_entry {
    override_summary summary{};
    std::int64_t bytes{};
    std::string sha256{};
};

struct registry {
    std::vector<index_entry> items{};
    std::vector<profile_binding> bindings{};
};

[[nodiscard]] auto qt_path(const fs::path& path) -> QString {
    return QString::fromStdWString(path.native());
}

[[nodiscard]] auto identifier() -> std::string {
    return QUuid::createUuid().toString(QUuid::Id128).toStdString();
}

[[nodiscard]] auto hash(std::string_view bytes) -> std::string {
    return QCryptographicHash::hash(
               QByteArrayView{std::to_address(bytes.begin()), std::ssize(bytes)},
               QCryptographicHash::Sha256)
        .toHex()
        .toStdString();
}

auto safe_path(const fs::path& root, const fs::path& path) -> void {
    const auto relative = path.lexically_relative(root);
    if (!path.is_absolute() || (relative.empty() && path != root)) {
        fail(override_error::unsafe_path);
    }
    for (const auto& part : relative) {
        if (part == fs::path{".."}) {
            fail(override_error::unsafe_path);
        }
    }
    // Include ancestors above the supplied data directory as well as children.
    auto current = path.root_path();
    for (const auto& part : path.relative_path()) {
        current /= part;
        const auto info = QFileInfo{qt_path(current)};
        if (info.isSymLink() || info.isJunction()) {
            fail(override_error::unsafe_path);
        }
    }
}

auto make_directory(const fs::path& root, const fs::path& path) -> void {
    safe_path(root, path);
    static_cast<void>(fs::create_directories(path));
    if (!fs::is_directory(path)) {
        fail(override_error::io_failure);
    }
}

[[nodiscard]] auto read_file(const fs::path& root, const fs::path& path, std::int64_t limit)
    -> std::string {
    safe_path(root, path);
    if (!fs::is_regular_file(path)) {
        fail(override_error::io_failure);
    }
    auto file = QFile{qt_path(path)};
    if (!file.open(QIODevice::ReadOnly)) {
        fail(override_error::io_failure);
    }
    const auto expected = file.size();
    if (expected < 0 || expected > limit) {
        fail(override_error::resource_limit);
    }
    const auto data = file.read(expected + 1);
    if (file.error() != QFileDevice::NoError || data.size() != expected || !file.atEnd()) {
        fail(override_error::io_failure);
    }
    return data.toStdString();
}

auto write_file(const fs::path& root, const fs::path& path, std::string_view bytes) -> void {
    safe_path(root, path);
    if (fs::exists(path) && !fs::is_regular_file(path)) {
        fail(override_error::unsafe_path);
    }
    auto file = QSaveFile{qt_path(path)};
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(std::to_address(bytes.begin()), std::ssize(bytes)) != std::ssize(bytes) ||
        !file.commit()) {
        fail(override_error::io_failure);
    }
}

[[nodiscard]] auto valid_label(std::string_view text) -> bool {
    constexpr auto label_limit = 4096;
    return !text.empty() && std::ssize(text) <= label_limit &&
           std::ranges::find(text, '\0') == text.end() && detail::valid_utf8(text);
}

auto validate_item(const override_item& item, const manager_options& options) -> void {
    if (std::ssize(item.content) > options.max_content_bytes) {
        fail(override_error::resource_limit);
    }
    if ((!item.id.empty() && !detail::valid_identifier(item.id)) || !valid_label(item.name) ||
        (item.ext != "yaml" && item.ext != "js") || !detail::valid_utf8(item.content)) {
        fail(override_error::invalid_input);
    }
}

[[nodiscard]] auto content_path(const manager_options& options, const override_summary& item)
    -> fs::path {
    return options.directory / "overrides" / item.id / (item.revision + "." + item.ext);
}

[[nodiscard]] auto read_content(const manager_options& options, const index_entry& entry)
    -> override_item {
    const auto& summary = entry.summary;
    auto text =
        read_file(options.directory, content_path(options, summary), options.max_content_bytes);
    if (std::ssize(text) != entry.bytes || hash(text) != entry.sha256) {
        fail(override_error::corrupt_content);
    }
    return {
        .id = summary.id,
        .name = summary.name,
        .ext = summary.ext,
        .is_global = summary.is_global,
        .content = std::move(text),
    };
}

[[nodiscard]] auto entry_json(const index_entry& entry) -> document {
    const auto& item = entry.summary;
    return {
        {"id", item.id},
        {"name", item.name},
        {"ext", item.ext},
        {"is_global", item.is_global},
        {"revision", item.revision},
        {"bytes", entry.bytes},
        {"sha256", entry.sha256},
    };
}

[[nodiscard]] auto encode_registry(const registry& index) -> std::string {
    auto result =
        document{{"version", 1}, {"items", document::array()}, {"bindings", document::array()}};
    for (const auto& item : index.items) {
        result.at("items").push_back(entry_json(item));
    }
    for (const auto& binding : index.bindings) {
        result.at("bindings")
            .push_back({
                {"profile_id", binding.profile_id},
                {"revision", binding.revision},
                {"override_ids", binding.override_ids},
            });
    }
    return result.dump();
}

[[nodiscard]] auto checked_entry(const document& value, const manager_options& options)
    -> index_entry {
    constexpr auto entry_fields = 7;
    constexpr auto digest_size = 64;
    if (!value.is_object() || value.size() != entry_fields || !value.at("is_global").is_boolean() ||
        !value.at("bytes").is_number_integer()) {
        fail(override_error::corrupt_index);
    }
    auto result = index_entry{
        .summary =
            {
                .id = value.at("id").get<std::string>(),
                .name = value.at("name").get<std::string>(),
                .ext = value.at("ext").get<std::string>(),
                .is_global = value.at("is_global").get<bool>(),
                .revision = value.at("revision").get<std::string>(),
            },
        .bytes = value.at("bytes").get<std::int64_t>(),
        .sha256 = value.at("sha256").get<std::string>(),
    };
    const auto& item = result.summary;
    if (!detail::valid_identifier(item.id) || !detail::valid_identifier(item.revision) ||
        !valid_label(item.name) || (item.ext != "yaml" && item.ext != "js") || result.bytes < 0 ||
        result.bytes > options.max_content_bytes || result.sha256.size() != digest_size ||
        !detail::valid_identifier(std::string_view{result.sha256}.substr(0, digest_size / 2)) ||
        !detail::valid_identifier(std::string_view{result.sha256}.substr(digest_size / 2))) {
        fail(override_error::corrupt_index);
    }
    return result;
}

[[nodiscard]] auto decode_registry(std::string_view text, const manager_options& options)
    -> registry {
    const auto limits = engine_options{.max_document_bytes = options.max_index_bytes};
    const auto value = detail::parse_json(text, limits);
    constexpr auto root_fields = 3;
    if (!value.contains("version") || !value.at("version").is_number_integer()) {
        fail(override_error::corrupt_index);
    }
    if (value.at("version") != 1) {
        fail(override_error::unsupported_version);
    }
    if (value.size() != root_fields || !value.at("items").is_array() ||
        !value.at("bindings").is_array()) {
        fail(override_error::corrupt_index);
    }
    if (std::cmp_greater(value.at("items").size(), options.max_items) ||
        std::cmp_greater(value.at("bindings").size(), options.max_bindings)) {
        fail(override_error::resource_limit);
    }
    auto result = registry{};
    auto ids = std::unordered_set<std::string>{};
    for (const auto& item : value.at("items")) {
        auto entry = checked_entry(item, options);
        if (!ids.insert(entry.summary.id).second) {
            fail(override_error::corrupt_index);
        }
        result.items.push_back(std::move(entry));
    }
    auto profiles = std::unordered_set<std::string>{};
    for (const auto& item : value.at("bindings")) {
        if (!item.is_object() || item.size() != root_fields ||
            !item.at("override_ids").is_array()) {
            fail(override_error::corrupt_index);
        }
        auto binding = profile_binding{
            .profile_id = item.at("profile_id").get<std::string>(),
            .revision = item.at("revision").get<std::string>(),
            .override_ids = item.at("override_ids").get<std::vector<std::string>>(),
        };
        if (!valid_label(binding.profile_id) || !detail::valid_identifier(binding.revision) ||
            !profiles.insert(binding.profile_id).second ||
            std::ssize(binding.override_ids) > options.max_selection) {
            fail(override_error::corrupt_index);
        }
        auto selected = std::unordered_set<std::string>{};
        for (const auto& selected_id : binding.override_ids) {
            if (!ids.contains(selected_id) || !selected.insert(selected_id).second) {
                fail(override_error::corrupt_index);
            }
        }
        result.bindings.push_back(std::move(binding));
    }
    return result;
}

class locked_store {
  public:
    explicit locked_store(const manager_options& selected)
        : options{selected}, lock{qt_path(selected.directory / "overrides.lock")} {
        make_directory(selected.directory, selected.directory);
        safe_path(selected.directory, selected.directory / "overrides.lock");
        lock.setStaleLockTime(0);
        if (!lock.tryLock(static_cast<int>(selected.lock_timeout.count()))) {
            fail(lock.error() == QLockFile::LockFailedError ? override_error::busy
                                                            : override_error::io_failure);
        }
    }

    [[nodiscard]] auto read_index() const -> registry {
        const auto& settings = options.get();
        const auto path = settings.directory / "overrides.json";
        safe_path(settings.directory, path);
        if (!fs::exists(path)) {
            return {};
        }
        const auto bytes = read_file(settings.directory, path, settings.max_index_bytes);
        try {
            return decode_registry(bytes, settings);
        } catch (const detail::override_failure& failure) {
            if (failure.code() == override_error::invalid_document) {
                fail(override_error::corrupt_index);
            }
            throw;
        } catch (const document::exception&) {
            throw detail::override_failure{override_error::corrupt_index};
        }
    }

    auto commit(const registry& index) const -> void {
        const auto bytes = encode_registry(index);
        const auto& settings = options.get();
        if (std::ssize(bytes) > settings.max_index_bytes) {
            fail(override_error::resource_limit);
        }
        write_file(settings.directory, settings.directory / "overrides.json", bytes);
    }

  private:
    std::reference_wrapper<const manager_options> options;
    QLockFile lock;
};

[[nodiscard]] auto find_entry(const registry& index, std::string_view item_id)
    -> const index_entry& {
    if (!detail::valid_identifier(item_id)) {
        fail(override_error::invalid_input);
    }
    const auto found =
        std::ranges::find_if(index.items, [item_id](const auto& entry) noexcept -> bool {
            return entry.summary.id == item_id;
        });
    if (found == index.items.end()) {
        fail(override_error::not_found);
    }
    return *found;
}

[[nodiscard]] auto binding_for(const registry& index, std::string_view profile_id)
    -> profile_binding {
    if (!valid_label(profile_id)) {
        fail(override_error::invalid_input);
    }
    const auto found = std::ranges::find(index.bindings, profile_id, &profile_binding::profile_id);
    return found == index.bindings.end() ? profile_binding{.profile_id = std::string{profile_id}}
                                         : *found;
}

[[nodiscard]] auto list_items(const manager_options& options) -> std::vector<override_summary> {
    const auto session = locked_store{options};
    auto index = session.read_index();
    auto result = std::vector<override_summary>{};
    result.reserve(index.items.size());
    std::ranges::transform(index.items, std::back_inserter(result),
                           [](auto& item) -> override_summary { return std::move(item.summary); });
    return result;
}

[[nodiscard]] auto get_item(const manager_options& options, std::string_view item_id)
    -> stored_override {
    const auto session = locked_store{options};
    const auto index = session.read_index();
    const auto& entry = find_entry(index, item_id);
    return {.item = read_content(options, entry), .revision = entry.summary.revision};
}

auto stage_content(const manager_options& options, const index_entry& entry, std::string_view text)
    -> void {
    const auto path = content_path(options, entry.summary);
    make_directory(options.directory, path.parent_path());
    if (fs::exists(path)) {
        fail(override_error::conflict);
    }
    write_file(options.directory, path, text);
}

[[nodiscard]] auto make_entry(const override_item& item) -> index_entry {
    return {
        .summary =
            {
                .id = item.id.empty() ? identifier() : item.id,
                .name = item.name,
                .ext = item.ext,
                .is_global = item.is_global,
                .revision = identifier(),
            },
        .bytes = std::ssize(item.content),
        .sha256 = hash(item.content),
    };
}

[[nodiscard]] auto save_item(const manager_options& options, const override_item& item,
                             std::string_view expected_revision, bool updating) -> stored_override {
    validate_item(item, options);
    auto entry = make_entry(item);
    // Allocate the returned object before publication, so allocation failure cannot
    // turn a successfully committed write into an apparent failed operation.
    auto result = stored_override{.item = item, .revision = entry.summary.revision};
    result.item.id = entry.summary.id;
    const auto session = locked_store{options};
    auto index = session.read_index();
    if (updating) {
        const auto& previous = find_entry(index, item.id);
        if (previous.summary.revision != expected_revision) {
            fail(override_error::conflict);
        }
        const auto found =
            std::ranges::find_if(index.items, [&item](const auto& stored) noexcept -> bool {
                return stored.summary.id == item.id;
            });
        *found = entry;
    } else {
        if (std::ranges::any_of(index.items, [&entry](const auto& previous) noexcept -> bool {
                return previous.summary.id == entry.summary.id;
            })) {
            fail(override_error::conflict);
        }
        if (std::ssize(index.items) >= options.max_items) {
            fail(override_error::resource_limit);
        }
        index.items.push_back(entry);
    }
    stage_content(options, entry, item.content);
    session.commit(index);
    return result;
}

[[nodiscard]] auto get_binding(const manager_options& options, std::string_view profile_id)
    -> profile_binding {
    const auto session = locked_store{options};
    return binding_for(session.read_index(), profile_id);
}

[[nodiscard]] auto set_binding(const manager_options& options, const profile_binding& binding)
    -> profile_binding {
    if (!valid_label(binding.profile_id) ||
        (!binding.revision.empty() && !detail::valid_identifier(binding.revision))) {
        fail(override_error::invalid_input);
    }
    if (std::ssize(binding.override_ids) > options.max_selection) {
        fail(override_error::resource_limit);
    }
    const auto session = locked_store{options};
    auto index = session.read_index();
    const auto previous = binding_for(index, binding.profile_id);
    if (previous.revision != binding.revision) {
        fail(override_error::conflict);
    }
    auto selected = std::unordered_set<std::string>{};
    for (const auto& item_id : binding.override_ids) {
        static_cast<void>(find_entry(index, item_id));
        if (!selected.insert(item_id).second) {
            fail(override_error::invalid_input);
        }
    }
    auto result = binding;
    result.revision = identifier();
    const auto found =
        std::ranges::find(index.bindings, binding.profile_id, &profile_binding::profile_id);
    if (found == index.bindings.end()) {
        if (std::ssize(index.bindings) >= options.max_bindings) {
            fail(override_error::resource_limit);
        }
        index.bindings.push_back(result);
    } else {
        *found = result;
    }
    session.commit(index);
    return result;
}

[[nodiscard]] auto load_selected(const manager_options& options, std::string_view profile_id)
    -> selection_snapshot {
    const auto session = locked_store{options};
    const auto index = session.read_index();
    const auto binding = binding_for(index, profile_id);
    auto selected = std::unordered_set<std::string>{};
    auto entries = std::vector<std::reference_wrapper<const index_entry>>{};
    for (const auto& entry : index.items) {
        if (entry.summary.is_global) {
            selected.insert(entry.summary.id);
            entries.push_back(std::cref(entry));
        }
    }
    for (const auto& item_id : binding.override_ids) {
        if (selected.insert(item_id).second) {
            entries.push_back(std::cref(find_entry(index, item_id)));
        }
    }
    if (std::ssize(entries) > options.max_selection) {
        fail(override_error::resource_limit);
    }
    auto bytes = std::int64_t{};
    auto result = selection_snapshot{.binding = binding};
    result.items.reserve(entries.size());
    for (const auto& entry : entries) {
        if (entry.get().bytes > options.max_loaded_bytes - bytes) {
            fail(override_error::resource_limit);
        }
        bytes += entry.get().bytes;
        result.items.push_back(
            {.item = read_content(options, entry), .revision = entry.get().summary.revision});
    }
    return result;
}

[[nodiscard]] auto load_items(const manager_options& options, std::string_view profile_id)
    -> std::vector<override_item> {
    auto selected = load_selected(options, profile_id);
    auto result = std::vector<override_item>{};
    result.reserve(selected.items.size());
    std::ranges::transform(
        selected.items, std::back_inserter(result),
        [](stored_override& item) -> override_item { return std::move(item.item); },
        std::identity{});
    return result;
}

[[nodiscard]] auto content_filename(const fs::path& path) -> bool {
    return detail::valid_identifier(path.stem().string()) &&
           (path.extension() == ".js" || path.extension() == ".yaml");
}

[[nodiscard]] auto directory_empty(const fs::path& path) -> bool {
    const auto contents = fs::directory_iterator{path};
    return contents == fs::directory_iterator{};
}

[[nodiscard]] auto clean_directory(const manager_options& options, const fs::path& directory,
                                   const fs::path& retained) noexcept -> cleanup_result {
    auto result = cleanup_result{};
    try {
        safe_path(options.directory, directory);
        if (!fs::exists(directory)) {
            return result;
        }
        for (const auto& file : fs::directory_iterator{directory}) {
            safe_path(options.directory, file.path());
            if (file.path() == retained) {
                continue;
            }
            if (!file.is_regular_file() || !content_filename(file.path())) {
                result.cleanup_pending = true;
                continue;
            }
            auto error = std::error_code{};
            if (fs::remove(file.path(), error)) {
                ++result.removed_files;
            }
            result.cleanup_pending = result.cleanup_pending || static_cast<bool>(error);
        }
        if (retained.empty() && directory_empty(directory)) {
            auto error = std::error_code{};
            static_cast<void>(fs::remove(directory, error));
            result.cleanup_pending = result.cleanup_pending || static_cast<bool>(error);
        }
    } catch (...) {
        result.cleanup_pending = true;
    }
    return result;
}

[[nodiscard]] auto remove_item(const manager_options& options, std::string_view item_id,
                               std::string_view expected_revision) -> cleanup_result {
    const auto session = locked_store{options};
    auto index = session.read_index();
    if (find_entry(index, item_id).summary.revision != expected_revision) {
        fail(override_error::conflict);
    }
    const auto directory = options.directory / "overrides" / std::string{item_id};
    std::erase_if(index.items, [item_id](const auto& entry) noexcept -> bool {
        return entry.summary.id == item_id;
    });
    for (auto& binding : index.bindings) {
        if (std::erase(binding.override_ids, item_id) != 0) {
            binding.revision = identifier();
        }
    }
    session.commit(index);
    return clean_directory(options, directory, {});
}

[[nodiscard]] auto collect(const manager_options& options) -> cleanup_result {
    const auto session = locked_store{options};
    const auto index = session.read_index();
    // Verify published content before deleting any potentially recoverable version.
    for (const auto& entry : index.items) {
        static_cast<void>(read_content(options, entry));
    }
    const auto parent = options.directory / "overrides";
    safe_path(options.directory, parent);
    auto result = cleanup_result{};
    if (!fs::exists(parent)) {
        return result;
    }
    for (const auto& directory : fs::directory_iterator{parent}) {
        const auto item_id = directory.path().filename().string();
        if (!detail::valid_identifier(item_id)) {
            continue;
        }
        safe_path(options.directory, directory.path());
        const auto found =
            std::ranges::find_if(index.items, [&item_id](const auto& entry) noexcept -> bool {
                return entry.summary.id == item_id;
            });
        const auto retained =
            found == index.items.end() ? fs::path{} : content_path(options, found->summary);
        const auto cleaned = clean_directory(options, directory.path(), retained);
        result.cleanup_pending = result.cleanup_pending || cleaned.cleanup_pending;
        result.removed_files += cleaned.removed_files;
    }
    return result;
}

[[nodiscard]] auto checked_options(manager_options options) -> manager_options {
    if (options.directory.empty() || !options.directory.is_absolute() ||
        options.lock_timeout.count() < 0 || options.lock_timeout > std::chrono::minutes{1}) {
        throw std::invalid_argument{"Invalid override storage directory or lock timeout"};
    }
    for (const auto limit : {
             options.max_items,
             options.max_bindings,
             options.max_index_bytes,
             options.max_content_bytes,
             options.max_selection,
             options.max_loaded_bytes,
         }) {
        if (limit <= 0 || limit >= std::numeric_limits<int>::max()) {
            throw std::invalid_argument{"Invalid override storage limit"};
        }
    }
    options.directory = options.directory.lexically_normal();
    return options;
}
// NOLINTEND(llvm-prefer-static-over-anonymous-namespace,altera-struct-pack-align,readability-redundant-member-init,altera-unroll-loops,fuchsia-default-arguments-calls)
} // namespace

override_manager::override_manager(manager_options selected)
    : options{checked_options(std::move(selected))} {}

auto override_manager::list() const noexcept -> override_result<std::vector<override_summary>> {
    return detail::guarded<std::vector<override_summary>>(list_items, options);
}
auto override_manager::get(std::string_view item_id) const noexcept
    -> override_result<stored_override> {
    return detail::guarded<stored_override>(get_item, options, item_id);
}
auto override_manager::create(const override_item& item) const noexcept
    -> override_result<stored_override> {
    return detail::guarded<stored_override>(save_item, options, item, std::string_view{}, false);
}
auto override_manager::update(const override_item& item,
                              std::string_view expected_revision) const noexcept
    -> override_result<stored_override> {
    return detail::guarded<stored_override>(save_item, options, item, expected_revision, true);
}
auto override_manager::remove(std::string_view item_id,
                              std::string_view expected_revision) const noexcept
    -> override_result<cleanup_result> {
    return detail::guarded<cleanup_result>(remove_item, options, item_id, expected_revision);
}
auto override_manager::get_profile_binding(std::string_view profile_id) const noexcept
    -> override_result<profile_binding> {
    return detail::guarded<profile_binding>(get_binding, options, profile_id);
}
auto override_manager::set_profile_binding(const profile_binding& binding) const noexcept
    -> override_result<profile_binding> {
    return detail::guarded<profile_binding>(set_binding, options, binding);
}
auto override_manager::load_for_profile(std::string_view profile_id) const noexcept
    -> override_result<std::vector<override_item>> {
    return detail::guarded<std::vector<override_item>>(load_items, options, profile_id);
}

auto override_manager::load_selection(std::string_view profile_id) const noexcept
    -> override_result<selection_snapshot> {
    return detail::guarded<selection_snapshot>(load_selected, options, profile_id);
}
auto override_manager::collect_garbage() const noexcept -> override_result<cleanup_result> {
    return detail::guarded<cleanup_result>(collect, options);
}

} // namespace overrides
