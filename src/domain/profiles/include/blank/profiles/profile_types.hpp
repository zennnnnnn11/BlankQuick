#ifndef BLANK_QUICK_PROFILE_TYPES_HPP // NOLINT(llvm-header-guard)
#define BLANK_QUICK_PROFILE_TYPES_HPP

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
    std::filesystem::path directory{};
    std::chrono::milliseconds lock_timeout{default_lock_timeout};
    std::int64_t max_profiles{default_profile_limit};
    std::int64_t max_index_bytes{default_index_limit};
    std::int64_t max_raw_bytes{default_raw_limit};
    std::int64_t max_cache_bytes{default_cache_limit};
    std::int64_t max_nodes{default_cache_node_limit};
};

struct profile_settings {
    std::string name{};
    std::string subscription_url{};
    std::optional<std::chrono::seconds> auto_update_interval{};
    bool preserve_original_rules{};
};

// id 是稳定身份；revision 用于编辑冲突检查；generation 指向一对原文/缓存资产。
// 修改名称等元数据可以只换 revision，更新订阅正文才需要发布新的 generation。
// 两个摘要分别保护原文和解析缓存，加载时不能只相信索引里记录的节点数。
struct profile_record {
    std::string id{};
    std::string revision{};
    std::string generation{};
    profile_settings settings{};
    subscription_data::account_metadata account{};
    std::int64_t created_unix_seconds{};
    std::int64_t updated_unix_seconds{};
    std::int64_t checked_unix_seconds{};
    std::int64_t node_count{};
    std::int64_t raw_bytes{};
    std::int64_t cache_bytes{};
    std::string raw_sha256{};
    std::string cache_sha256{};
};

// 写入调用期间借用输入文本和节点，不接管它们的所有权。调用者必须保持底层
// 缓冲区存活到同步操作返回；长期保存和异步传递应使用拥有数据的 snapshot。
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
    subscription_data::account_metadata account{};
};

struct node_snapshot {
    profile_record profile{};
    std::vector<node_model::unified_node> nodes{};
};

struct raw_snapshot {
    profile_record profile{};
    std::string raw{};
};

// 一次加载同时取得索引身份、原文和节点，确保它们属于同一存储世代。
// 应用层以这份快照建立会话，不应分别读取原文和节点后自行拼接版本。
struct material_snapshot {
    profile_record profile{};
    std::vector<node_model::unified_node> nodes{};
    std::string raw{};
};

struct cleanup_result {
    bool cleanup_pending{};
    std::int64_t removed_directories{};
};
// NOLINTEND(altera-struct-pack-align, readability-redundant-member-init)

[[nodiscard]] [[maybe_unused]] auto used_bytes(const profile_record& profile) noexcept
    -> std::optional<std::int64_t>;

} // namespace profile_storage

#endif
