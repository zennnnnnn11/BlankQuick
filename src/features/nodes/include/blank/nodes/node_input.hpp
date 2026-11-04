#ifndef BLANK_QUICK_NODE_INPUT_HPP // NOLINT(llvm-header-guard)
#define BLANK_QUICK_NODE_INPUT_HPP

#include <blank/nodes/unified_node.hpp>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace node_input {

enum class parse_error : std::int8_t {
    none,
    unsupported_protocol,
    invalid_uri,
    invalid_encoding,
    invalid_endpoint,
    missing_credential,
    invalid_parameter,
    unsupported_parameter,
    resource_limit,
    allocation_failure,
    unexpected_failure,
    invalid_document,
};

inline constexpr std::int64_t default_input_bytes{33554432};
inline constexpr std::int64_t default_line_bytes{262144};
inline constexpr std::int64_t default_node_limit{100000};
inline constexpr std::int64_t default_line_limit{200000};
inline constexpr std::int64_t default_parameter_limit{512};
inline constexpr std::int64_t default_json_depth{32};
inline constexpr std::int64_t default_yaml_nodes{1000000};
inline constexpr std::int64_t default_expanded_values{2000000};
// NOLINTBEGIN(altera-struct-pack-align, readability-redundant-member-init)
struct parse_limits {
    std::int64_t max_input_bytes{default_input_bytes};
    std::int64_t max_line_bytes{default_line_bytes};
    std::int64_t max_nodes{default_node_limit};
    std::int64_t max_lines{default_line_limit};
    std::int64_t max_parameters{default_parameter_limit};
    std::int64_t max_json_depth{default_json_depth};
    std::int64_t max_yaml_nodes{default_yaml_nodes};
    std::int64_t max_expanded_values{default_expanded_values};
};

struct node_parse_result {
    std::optional<node_model::unified_node> node{};
    parse_error error{parse_error::none};
    std::vector<std::string> ignored_parameters{};
};

enum class node_collection : std::int8_t { lines, proxies, outbounds, endpoints, peers, nodes };

struct line_diagnostic {
    std::int64_t line{};
    parse_error error{parse_error::none};
    std::vector<std::string> ignored_parameters{};
    std::int64_t index{};
    node_collection collection{node_collection::lines};
};

enum class subscription_encoding : std::int8_t { plain_text, base64, double_base64 };
enum class subscription_format : std::int8_t {
    uri_lines,
    clash_yaml,
    sing_box_json,
    wireguard_conf,
};

struct subscription_result {
    std::vector<node_model::unified_node> nodes{};
    std::vector<line_diagnostic> diagnostics{};
    subscription_encoding encoding{subscription_encoding::plain_text};
    parse_error error{parse_error::none};
    subscription_format format{subscription_format::uri_lines};
};
// NOLINTEND(altera-struct-pack-align, readability-redundant-member-init)

// NOLINTBEGIN(fuchsia-default-arguments-declarations)
[[nodiscard]] auto parse_uri(std::string_view input, const parse_limits& limits = {}) noexcept
    -> node_parse_result;
[[nodiscard]] auto parse_subscription(std::string_view input,
                                      const parse_limits& limits = {}) noexcept
    -> subscription_result;
// NOLINTEND(fuchsia-default-arguments-declarations)

} // namespace node_input

#endif
