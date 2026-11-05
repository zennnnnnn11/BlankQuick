#ifndef BLANK_QUICK_PROFILE_STORAGE_HPP // NOLINT(llvm-header-guard)
#define BLANK_QUICK_PROFILE_STORAGE_HPP

#include <blank/profiles/profile_types.hpp>

namespace profile_storage {

[[nodiscard]] [[maybe_unused]] auto default_data_directory() -> std::filesystem::path;

// 配置资产的同步存储入口。它可以保存已获取的数据，却不负责联网、生成运行
// 配置或启停内核；这些跨界动作必须由应用层安排。
// 对已有记录的修改显式携带 expected_revision，冲突时返回 conflict，不静默覆盖现场。
class profile_manager {
  public:
    explicit profile_manager(storage_options selected);

    [[nodiscard]] auto list() const noexcept -> storage_result<std::vector<profile_record>>;
    [[nodiscard]] auto get(std::string_view profile_id) const noexcept
        -> storage_result<profile_record>;
    [[nodiscard]] [[maybe_unused]] auto load_nodes(std::string_view profile_id) const noexcept
        -> storage_result<node_snapshot>;
    [[nodiscard]] [[maybe_unused]] auto load_raw(std::string_view profile_id) const noexcept
        -> storage_result<raw_snapshot>;
    // 需要激活时优先使用这一入口：身份、原文、节点在同一锁定快照下捕获。
    [[nodiscard]] auto load_material(std::string_view profile_id) const noexcept
        -> storage_result<material_snapshot>;
    [[nodiscard]] auto save_new(const profile_input& input) const noexcept
        -> storage_result<profile_record>;
    [[nodiscard]] [[maybe_unused]] auto
    save_fetched(profile_settings settings,
                 const subscription_data::fetch_result& fetched) const noexcept
        -> storage_result<profile_record>;
    [[nodiscard]] auto replace(const profile_refresh& input) const noexcept
        -> storage_result<profile_record>;
    [[nodiscard]] [[maybe_unused]] auto edit(const profile_edit& input) const noexcept
        -> storage_result<profile_record>;
    [[nodiscard]] [[maybe_unused]] auto update_metadata(const metadata_update& input) const noexcept
        -> storage_result<profile_record>;
    [[nodiscard]] [[maybe_unused]] auto
    apply_fetch(std::string_view profile_id, std::string_view expected_revision,
                const subscription_data::fetch_result& fetched) const noexcept
        -> storage_result<profile_record>;
    [[nodiscard]] auto remove(std::string_view profile_id,
                              std::string_view expected_revision) const noexcept
        -> storage_result<cleanup_result>;
    [[nodiscard]] [[maybe_unused]] auto collect_garbage() const noexcept
        -> storage_result<cleanup_result>;

  private:
    storage_options options;
    std::shared_ptr<detail::validation_state> validation;
};

} // namespace profile_storage

#endif
