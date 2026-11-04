#include "unified_node.hpp"

#include <type_traits>
#include <utility>

static_assert(std::is_aggregate_v<node_model::unified_node>);
static_assert(
    noexcept(node_model::has_matching_protocol(std::declval<const node_model::unified_node&>())));
static_assert(node_model::kernel_protocol_name(node_model::protocol_type::shadowsocks) == "ss");
