#ifndef BLANK_QUICK_CLASH_MATERIAL_HPP // NOLINT(llvm-header-guard)
#define BLANK_QUICK_CLASH_MATERIAL_HPP

#include <blank/nodes/node_input.hpp>
#include <nlohmann/json.hpp>
#include <string_view>

// Document boundary for profile assembly; parsing internals remain in the nodes slice.
namespace node_input::material {

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

} // namespace node_input::material

#endif
