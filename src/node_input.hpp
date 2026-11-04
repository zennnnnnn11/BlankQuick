#ifndef BLANK_QUICK_NODE_INPUT_HPP // NOLINT(llvm-header-guard)
#define BLANK_QUICK_NODE_INPUT_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "unified_node.hpp"

namespace node_input {

// 错误和警告不保存原始链接、密码或密钥，可直接用于导入结果展示。
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

// 长度单位为字节；行数及节点数限制也约束恶意订阅的内存占用。
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
    std::int64_t line{}; // 解码后文本的 1 基行号；JSON 无物理行号时为 0。
    parse_error error{parse_error::none};
    std::vector<std::string> ignored_parameters{};
    std::int64_t index{}; // 集合中从 1 开始的位置；JSON 不伪造物理行号。
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
    parse_error error{parse_error::none}; // 仅表示整包错误；节点错误在 diagnostics。
    subscription_format format{subscription_format::uri_lines};
};
// NOLINTEND(altera-struct-pack-align, readability-redundant-member-init)

// 单链接、明文/Base64 订阅、Clash YAML、Sing-box JSON 和 WireGuard conf。
// 两个入口都截获解析和分配异常；批量模式保留出错前已经成功装配的节点。
// ISO C++ 默认参数用于可选资源限制，不遵循 Fuchsia 的禁用默认参数风格。
// NOLINTBEGIN(fuchsia-default-arguments-declarations)
[[nodiscard]] auto parse_uri(std::string_view input, const parse_limits& limits = {}) noexcept
    -> node_parse_result;
[[nodiscard]] auto parse_subscription(std::string_view input,
                                      const parse_limits& limits = {}) noexcept
    -> subscription_result;
// NOLINTEND(fuchsia-default-arguments-declarations)

} // namespace node_input

#endif
