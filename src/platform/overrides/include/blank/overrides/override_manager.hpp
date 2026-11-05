#ifndef BLANK_QUICK_OVERRIDE_MANAGER_HPP // NOLINT(llvm-header-guard)
#define BLANK_QUICK_OVERRIDE_MANAGER_HPP

#include <blank/overrides/override_types.hpp>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace overrides {

inline constexpr std::chrono::milliseconds default_manager_lock_timeout{5000};
inline constexpr std::int64_t default_manager_item_limit{10000};
inline constexpr std::int64_t default_manager_index_bytes{16777216};
inline constexpr std::int64_t default_manager_content_bytes{2097152};
inline constexpr std::int64_t default_manager_selection_limit{128};
inline constexpr std::int64_t default_manager_loaded_bytes{33554432};

// NOLINTBEGIN(altera-struct-pack-align,readability-redundant-member-init)
struct manager_options {
    std::filesystem::path directory{};
    std::chrono::milliseconds lock_timeout{default_manager_lock_timeout};
    std::int64_t max_items{default_manager_item_limit};
    std::int64_t max_bindings{default_manager_item_limit};
    std::int64_t max_index_bytes{default_manager_index_bytes};
    std::int64_t max_content_bytes{default_manager_content_bytes};
    std::int64_t max_selection{default_manager_selection_limit};
    std::int64_t max_loaded_bytes{default_manager_loaded_bytes};
};

struct override_summary {
    std::string id{};
    std::string name{};
    std::string ext{};
    bool is_global{};
    std::string revision{};
};

struct stored_override {
    override_item item{};
    std::string revision{};
};

struct profile_binding {
    std::string profile_id{};
    std::string revision{};
    std::vector<std::string> override_ids{};
};

// 同一文件锁下读取绑定和内容版本。items 已按“全局覆写在前、绑定顺序在后”
// 排好并去重，应用层应保持该顺序，不再自行扫描管理器补齐内容。
struct selection_snapshot {
    profile_binding binding{};
    std::vector<stored_override> items{};
};

struct cleanup_result {
    bool cleanup_pending{};
    std::int64_t removed_files{};
};
// NOLINTEND(altera-struct-pack-align,readability-redundant-member-init)

// 只管理覆写资产和绑定，不执行脚本、不改变内核。revision 检查用于拒绝
// 过期编辑；内容先按新版本写入，再原子发布索引，旧内容交给垃圾回收。
class override_manager {
  public:
    explicit override_manager(manager_options selected);

    [[nodiscard]] auto list() const noexcept -> override_result<std::vector<override_summary>>;
    [[nodiscard]] auto get(std::string_view item_id) const noexcept
        -> override_result<stored_override>;

    [[nodiscard]] auto create(const override_item& item) const noexcept
        -> override_result<stored_override>;
    [[nodiscard]] auto update(const override_item& item,
                              std::string_view expected_revision) const noexcept
        -> override_result<stored_override>;
    [[nodiscard]] auto remove(std::string_view item_id,
                              std::string_view expected_revision) const noexcept
        -> override_result<cleanup_result>;
    [[nodiscard]] [[maybe_unused]] auto
    get_profile_binding(std::string_view profile_id) const noexcept
        -> override_result<profile_binding>;

    [[nodiscard]] auto set_profile_binding(const profile_binding& binding) const noexcept
        -> override_result<profile_binding>;

    [[nodiscard]] [[maybe_unused]] auto load_for_profile(std::string_view profile_id) const noexcept
        -> override_result<std::vector<override_item>>;
    [[nodiscard]] auto load_selection(std::string_view profile_id) const noexcept
        -> override_result<selection_snapshot>;
    [[nodiscard]] [[maybe_unused]] auto collect_garbage() const noexcept
        -> override_result<cleanup_result>;

  private:
    manager_options options;
};

} // namespace overrides

#endif
