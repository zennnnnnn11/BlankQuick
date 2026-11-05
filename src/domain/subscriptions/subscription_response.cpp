#include <algorithm>
#include <blank/nodes/node_input.hpp>
#include <blank/subscriptions/subscription_account.hpp>
#include <blank/subscriptions/subscription_data.hpp>
#include <blank/subscriptions/subscription_metadata.hpp>
#include <blank/subscriptions/subscription_response.hpp>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

namespace subscription_data {
namespace {
// NOLINTBEGIN(llvm-prefer-static-over-anonymous-namespace,altera-unroll-loops,fuchsia-default-arguments-calls)
[[nodiscard]] auto looks_like_html(std::string_view body) -> bool {
    constexpr std::size_t preview_bytes{512};
    auto preview = std::string{body.substr(0, preview_bytes)};
    std::ranges::transform(preview, preview.begin(), [](char character) noexcept -> char {
        return character >= 'A' && character <= 'Z' ? static_cast<char>(character + ('a' - 'A'))
                                                    : character;
    });
    const auto first = preview.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return false;
    }
    const auto markup = std::string_view{preview}.substr(first);
    return markup.starts_with("<!doctype") || markup.starts_with("<html") ||
           markup.starts_with("<head");
}
// NOLINTEND(llvm-prefer-static-over-anonymous-namespace,altera-unroll-loops,fuchsia-default-arguments-calls)
} // namespace

auto decode_response(http_response&& response, const response_options& options) -> fetch_result {
    // 先检查响应头，再解释状态和正文：即使是 304，也可能有新的配额和缓存标记。
    // 头部资源上限属于整次获取的终止条件，不能被另一条代理路由“重试掉”。
    auto received = std::move(response);
    auto result = fetch_result{};
    result.http_status = received.status;
    result.metadata =
        subscription_fetcher::parse_response_headers(received.headers, options.headers);
    if (result.metadata.error == metadata_error::allocation_failure) {
        throw response_failure{fetch_error::allocation_failure};
    }
    if (result.metadata.error == metadata_error::resource_limit) {
        throw response_failure{fetch_error::headers_too_large};
    }
    constexpr auto not_modified = 304;
    constexpr auto success = 200;
    constexpr auto no_content = 204;
    constexpr auto proxy_authentication = 407;
    if (received.status == not_modified) {
        result.not_modified = true;
        return result;
    }
    if (received.status != success) {
        if (received.status == no_content) {
            result.error = fetch_error::empty_body;
        } else if (received.status == proxy_authentication) {
            result.error = fetch_error::proxy_failure;
        } else {
            result.error = fetch_error::http_status;
        }
        return result;
    }
    if (received.body.empty() || std::ranges::all_of(
                                     received.body,
                                     [](char character) noexcept -> bool {
                                         return character == ' ' || character == '\t' ||
                                                character == '\r' || character == '\n';
                                     },
                                     std::identity{})) {
        result.error = fetch_error::empty_body;
        return result;
    }
    // 订阅地址可能返回登录页或网关错误页。先给出内容类型诊断，避免把它们
    // 送入节点解析器后报告成大量无意义的分享链接错误。
    if (looks_like_html(received.body)) {
        result.error = fetch_error::unexpected_content;
        return result;
    }
    result.subscription = node_input::parse_subscription(received.body, options.parser);
    if (result.subscription.error == node_input::parse_error::allocation_failure) {
        result.error = fetch_error::allocation_failure;
    } else if (result.subscription.error != node_input::parse_error::none) {
        result.error = fetch_error::parse_failure;
    } else if (result.subscription.nodes.empty()) {
        result.error = fetch_error::no_supported_nodes;
    } else {
        result.raw_body = std::move(received.body);
    }
    return result;
}
} // namespace subscription_data
