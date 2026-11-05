#ifndef BLANK_QUICK_CLASH_MATERIAL_HPP // NOLINT(llvm-header-guard)
#define BLANK_QUICK_CLASH_MATERIAL_HPP

#include <blank/nodes/node_input.hpp>
#include <nlohmann/json.hpp>
#include <string_view>

namespace node_input::material {

// NOLINTBEGIN(altera-struct-pack-align,readability-redundant-member-init)
// 保留原规则时需要的文档片段，不是可直接执行的 Mihomo 配置。
// found 区分“不是 Clash 材料”和“找到了材料但内容/引用还需后续组装校验”；
// error 则说明提取本身失败，不能用空 sections 掩盖失败。
struct clash_material {

    // NOLINTNEXTLINE(fuchsia-default-arguments-calls)
    nlohmann::json sections{};
    parse_error error{parse_error::none};
    bool found{};
};
// NOLINTEND(altera-struct-pack-align,readability-redundant-member-init)

[[nodiscard]] auto extract_clash_material(std::string_view input,
                                          const parse_limits& limits) noexcept -> clash_material;

} // namespace node_input::material

#endif
