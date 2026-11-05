#ifndef BLANK_QUICK_SUBSCRIPTION_DATA_HPP // NOLINT(llvm-header-guard)
#define BLANK_QUICK_SUBSCRIPTION_DATA_HPP

#include <blank/nodes/node_input.hpp>
#include <blank/subscriptions/subscription_account.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace subscription_data {
// NOLINTBEGIN(altera-struct-pack-align,readability-redundant-member-init)
enum class fetch_error : std::int8_t {
    none,
    invalid_request,
    environment_unavailable,
    cancelled,
    timeout,
    dns_failure,
    connection_failure,
    proxy_failure,
    tls_failure,
    network_failure,
    http_status,
    invalid_redirect,
    too_many_redirects,
    body_too_large,
    headers_too_large,
    empty_body,
    unexpected_content,
    parse_failure,
    no_supported_nodes,
    allocation_failure,
    unexpected_failure,
};

struct fetch_attempt {
    std::int64_t route_index{};
    fetch_error error{fetch_error::none};
    std::int64_t http_status{};
};

// 保留传输事实与内容解释两部分。304 是成功的“正文未变”，不会携带新节点；
// 调用者应更新检查时间/元数据并继续使用旧资产，不能把空 raw_body 覆盖到存储。
// attempts 用于说明路由尝试过程，不参与决定应该启动或重启哪个内核。
struct fetch_result {
    node_input::subscription_result subscription{};
    std::string raw_body{};
    metadata_result metadata{};
    fetch_error error{fetch_error::none};
    std::int64_t http_status{};
    std::int64_t route_index{};
    std::int64_t redirect_count{};
    bool not_modified{};
    std::string final_url{};
    std::vector<fetch_attempt> attempts{};
};
// NOLINTEND(altera-struct-pack-align,readability-redundant-member-init)
} // namespace subscription_data

namespace subscription_fetcher {
using subscription_data::fetch_attempt;
using subscription_data::fetch_error;
using subscription_data::fetch_result;
} // namespace subscription_fetcher

#endif
