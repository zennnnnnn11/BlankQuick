#ifndef BLANK_QUICK_SETTINGS_STORAGE_HPP // NOLINT(llvm-header-guard)
#define BLANK_QUICK_SETTINGS_STORAGE_HPP

#include <blank/runtime_config/runtime_config.hpp>

namespace runtime_config {

// NOLINTBEGIN(altera-struct-pack-align,readability-redundant-member-init,fuchsia-default-arguments-declarations)
inline constexpr std::chrono::milliseconds default_lock_timeout{5000};
inline constexpr std::int64_t default_settings_bytes{131072};

enum class settings_error : std::int8_t {
    none,
    invalid_input,
    conflict,
    busy,
    unsafe_path,
    corrupt_settings,
    io_failure,
    allocation_failure,
    unexpected_failure,
};

// 空 revision 表示尚无持久化文件的默认设置。save 要求传回读取时的 revision，
// 在文件锁内比较后才发布新版本，防止两个编辑界面相互覆盖。
struct settings_snapshot {
    client_settings settings{};
    std::string revision{};
};

struct settings_result {
    std::optional<settings_snapshot> value{};
    settings_error error{settings_error::none};
};

struct settings_storage_options {
    std::filesystem::path directory{};
    std::chrono::milliseconds lock_timeout{default_lock_timeout};
    std::int64_t max_bytes{default_settings_bytes};
};

// 同步文件适配器，不决定何时把保存的设置应用到内核。保存成功只改变存储，
// 生效仍需应用层提交包含这份快照的激活事务。
class settings_manager {
  public:
    explicit settings_manager(settings_storage_options selected);
    [[nodiscard]] auto load() const noexcept -> settings_result;
    [[nodiscard]] auto save(const client_settings& settings,
                            std::string_view expected_revision) const noexcept -> settings_result;

  private:
    settings_storage_options options;
};

// NOLINTEND(altera-struct-pack-align,readability-redundant-member-init,fuchsia-default-arguments-declarations)
} // namespace runtime_config

#endif
