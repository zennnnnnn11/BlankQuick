#ifndef BLANK_QUICK_SUBSCRIPTION_DATA_HPP // NOLINT(llvm-header-guard)
#define BLANK_QUICK_SUBSCRIPTION_DATA_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "node_input.hpp"
#include "subscription_account.hpp"

// Owned subscription values shared by download, persistence and activation layers.
// This header has no network backend, worker or filesystem dependencies.
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
    std::int64_t route_index{}; // Zero based; no proxy credentials or token URL.
    fetch_error error{fetch_error::none};
    std::int64_t http_status{};
};

struct fetch_result {
    node_input::subscription_result subscription{};
    std::string raw_body{}; // Successful 200 document bytes before parsing; empty on failure/304.
    metadata_result metadata{};
    fetch_error error{fetch_error::none};
    std::int64_t http_status{};
    std::int64_t route_index{};
    std::int64_t redirect_count{};
    bool not_modified{};     // 304: retain the previously stored nodes and metadata.
    std::string final_url{}; // May contain a subscription token; not a log string.
    std::vector<fetch_attempt> attempts{};
};
// NOLINTEND(altera-struct-pack-align,readability-redundant-member-init)
} // namespace subscription_data

// Source compatibility for callers using the original transport namespace.
namespace subscription_fetcher {
using subscription_data::fetch_attempt;
using subscription_data::fetch_error;
using subscription_data::fetch_result;
} // namespace subscription_fetcher

#endif
