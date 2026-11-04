#ifndef BLANK_QUICK_CLASH_MATERIAL_HPP // NOLINT(llvm-header-guard)
#define BLANK_QUICK_CLASH_MATERIAL_HPP

#include <nlohmann/json.hpp>
#include <string_view>

#include "node_input.hpp"

// Private bridge: reuse the input layer's bounded YAML/alias/Base64 reader.
namespace node_input::detail {

// NOLINTBEGIN(altera-struct-pack-align,readability-redundant-member-init)
struct clash_material {
    // Canonical null JSON uses the library's standard default constructor.
    // NOLINTNEXTLINE(fuchsia-default-arguments-calls)
    nlohmann::json sections{};
    parse_error error{parse_error::none};
    bool found{};
};
// NOLINTEND(altera-struct-pack-align,readability-redundant-member-init)

[[nodiscard]] auto extract_clash_material(std::string_view input,
                                          const parse_limits& limits) noexcept -> clash_material;

} // namespace node_input::detail

#endif
