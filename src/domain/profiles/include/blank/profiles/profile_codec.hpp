#ifndef BLANK_QUICK_PROFILE_CODEC_HPP // NOLINT(llvm-header-guard)
#define BLANK_QUICK_PROFILE_CODEC_HPP

#include <blank/profiles/profile_types.hpp>
#include <exception>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace profile_storage::detail {

// Exception values retain standard copy semantics; this final leaf is caught by const reference.
// NOLINTNEXTLINE(altera-struct-pack-align)
class storage_failure final : public std::exception { // clazy:exclude=copyable-polymorphic
  public:
    explicit storage_failure(storage_error selected) noexcept : error{selected} {}
    [[nodiscard]] auto code() const noexcept -> storage_error {
        return error;
    }

  private:
    storage_error error{};
};

// 编解码只处理内存，摘要校验、路径安全与文件原子发布由存储适配器负责。
// 索引格式版本和节点 model_schema 各自校验；缓存不能因为 JSON 合法就被信任。
// 失败抛出 storage_failure 供平台入口统一转换，不在此处自行读取原文或修复磁盘。
[[nodiscard]] auto encode_index(std::span<const profile_record> records) -> std::string;
[[nodiscard]] auto decode_index(std::string_view text, std::int64_t max_profiles)
    -> std::vector<profile_record>;
[[nodiscard]] auto encode_nodes(std::span<const node_model::unified_node> nodes) -> std::string;
[[nodiscard]] auto decode_nodes(std::string_view text, std::int64_t max_nodes)
    -> std::vector<node_model::unified_node>;

} // namespace profile_storage::detail

#endif
