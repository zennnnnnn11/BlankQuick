#ifndef BLANK_QUICK_NODE_FILTER_HPP // NOLINT(llvm-header-guard)
#define BLANK_QUICK_NODE_FILTER_HPP

#include <blank/nodes/unified_node.hpp>
#include <cstdint>

namespace node_filter {

enum class informational_reason : std::int8_t {
    none,
    announcement_name,
    local_endpoint,
    missing_endpoint,
    placeholder_endpoint,
};

[[nodiscard]] auto classify_informational_node(const node_model::unified_node& node) noexcept
    -> informational_reason;

[[nodiscard]] inline auto is_informational_node(const node_model::unified_node& node) noexcept
    -> bool {
    return classify_informational_node(node) != informational_reason::none;
}

} // namespace node_filter

#endif
