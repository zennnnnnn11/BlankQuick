#ifndef BLANK_QUICK_PROFILE_STORAGE_HPP // NOLINT(llvm-header-guard)
#define BLANK_QUICK_PROFILE_STORAGE_HPP

#include <blank/nodes/unified_node.hpp>
#include <blank/subscriptions/subscription_account.hpp>
#include <blank/subscriptions/subscription_data.hpp>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace profile_storage {

namespace detail {
struct validation_state;
} // namespace detail

inline constexpr std::chrono::milliseconds default_lock_timeout{5000};
inline constexpr std::int64_t default_profile_limit{10000};
inline constexpr std::int64_t default_index_limit{16777216};
inline constexpr std::int64_t default_raw_limit{33554432};
inline constexpr std::int64_t default_cache_limit{268435456};
inline constexpr std::int64_t default_cache_node_limit{100000};

enum class storage_error : std::int8_t {
    none,
    invalid_input,
    not_found,
    conflict,
    busy,
    io_failure,
    unsafe_path,
    corrupt_index,
    corrupt_cache,
    corrupt_raw,
    unsupported_version,
    resource_limit,
    fetch_failed,
    allocation_failure,
    unexpected_failure,
};

// NOLINTBEGIN(altera-struct-pack-align, readability-redundant-member-init)
template <typename Value> struct storage_result {
    Value value{};
    storage_error error{storage_error::none};
};

struct storage_options {
    std::filesystem::path directory{}; // Absolute, dedicated local data directory.
    std::chrono::milliseconds lock_timeout{default_lock_timeout};
    std::int64_t max_profiles{default_profile_limit};
    std::int64_t max_index_bytes{default_index_limit};
    std::int64_t max_raw_bytes{default_raw_limit};
    std::int64_t max_cache_bytes{default_cache_limit};
    std::int64_t max_nodes{default_cache_node_limit};
};

struct profile_settings {
    std::string name{};
    std::string subscription_url{}; // Empty for a local import; may contain credentials.
    std::optional<std::chrono::seconds> auto_update_interval{}; // nullopt disables updates.
    bool preserve_original_rules{}; // Stored user choice; does not activate rules in this layer.
};

struct profile_record {
    std::string id{};
    std::string revision{};   // Optimistic concurrency token; changes on every edit.
    std::string generation{}; // Immutable raw.txt/nodes.json directory; private file layout.
    profile_settings settings{};
    subscription_data::account_metadata account{};
    std::int64_t created_unix_seconds{};
    std::int64_t updated_unix_seconds{}; // Last successful body replacement.
    std::int64_t checked_unix_seconds{}; // Also advances on a successful HTTP 304.
    std::int64_t node_count{};
    std::int64_t raw_bytes{};
    std::int64_t cache_bytes{};
    std::string raw_sha256{};
    std::string cache_sha256{};
};

// Borrowed inputs are consumed synchronously; no reference survives the call.
struct profile_input {
    profile_settings settings{};
    subscription_data::account_metadata account{};
    std::string_view raw{};
    std::span<const node_model::unified_node> nodes{};
};

struct profile_refresh {
    std::string id{};
    std::string expected_revision{};
    subscription_data::account_metadata account{};
    std::string_view raw{};
    std::span<const node_model::unified_node> nodes{};
};

struct profile_edit {
    std::string id{};
    std::string expected_revision{};
    profile_settings settings{};
};

struct metadata_update {
    std::string id{};
    std::string expected_revision{};
    subscription_data::account_metadata account{}; // Present values patch; absent values persist.
};

struct node_snapshot {
    profile_record profile{};
    std::vector<node_model::unified_node> nodes{};
};

struct raw_snapshot {
    profile_record profile{};
    std::string raw{}; // Exact response document bytes, including BOM/line endings.
};

// A single locked read of one published generation. No policy assembly occurs here.
struct material_snapshot {
    profile_record profile{};
    std::vector<node_model::unified_node> nodes{};
    std::string raw{};
};

struct cleanup_result {
    bool cleanup_pending{}; // Logical deletion has committed; physical retry may be needed.
    std::int64_t removed_directories{};
};
// NOLINTEND(altera-struct-pack-align, readability-redundant-member-init)

[[nodiscard]] auto default_data_directory() -> std::filesystem::path;
[[nodiscard]] auto used_bytes(const profile_record& profile) noexcept
    -> std::optional<std::int64_t>;

// Blocking worker API. No Qt/JSON types escape this header. Cooperating readers
// and writers serialize via an RAII file lock; cold starts never parse raw.txt.
class profile_manager {
  public:
    // Invalid options throw std::invalid_argument; no filesystem changes here.
    explicit profile_manager(storage_options selected);

    [[nodiscard]] auto list() const noexcept -> storage_result<std::vector<profile_record>>;
    [[nodiscard]] auto get(std::string_view profile_id) const noexcept
        -> storage_result<profile_record>;
    [[nodiscard]] auto load_nodes(std::string_view profile_id) const noexcept
        -> storage_result<node_snapshot>;
    [[nodiscard]] auto load_raw(std::string_view profile_id) const noexcept
        -> storage_result<raw_snapshot>;
    [[nodiscard]] auto load_material(std::string_view profile_id) const noexcept
        -> storage_result<material_snapshot>;
    [[nodiscard]] auto save_new(const profile_input& input) const noexcept
        -> storage_result<profile_record>;
    [[nodiscard]] auto save_fetched(profile_settings settings,
                                    const subscription_data::fetch_result& fetched) const noexcept
        -> storage_result<profile_record>;
    [[nodiscard]] auto replace(const profile_refresh& input) const noexcept
        -> storage_result<profile_record>;
    [[nodiscard]] auto edit(const profile_edit& input) const noexcept
        -> storage_result<profile_record>;
    [[nodiscard]] auto update_metadata(const metadata_update& input) const noexcept
        -> storage_result<profile_record>;
    [[nodiscard]] auto apply_fetch(std::string_view profile_id, std::string_view expected_revision,
                                   const subscription_data::fetch_result& fetched) const noexcept
        -> storage_result<profile_record>;
    [[nodiscard]] auto remove(std::string_view profile_id,
                              std::string_view expected_revision) const noexcept
        -> storage_result<cleanup_result>;
    [[nodiscard]] auto collect_garbage() const noexcept -> storage_result<cleanup_result>;

  private:
    storage_options options;
    std::shared_ptr<detail::validation_state> validation;
};

} // namespace profile_storage

#endif
