#ifndef BLANK_QUICK_SUBSCRIPTION_FETCHER_HPP // NOLINT(llvm-header-guard)
#define BLANK_QUICK_SUBSCRIPTION_FETCHER_HPP

#include <blank/nodes/node_input.hpp>
#include <blank/subscriptions/subscription_data.hpp>
#include <blank/subscriptions/subscription_metadata.hpp>
#include <chrono>
#include <cstdint>
#include <future>
#include <stop_token>
#include <string>
#include <vector>

namespace subscription_fetcher {

enum class proxy_mode : std::int8_t { system, direct, http, socks5 };

inline constexpr std::chrono::milliseconds default_total_timeout{60000};
inline constexpr std::chrono::milliseconds default_route_timeout{20000};
inline constexpr std::chrono::milliseconds default_idle_timeout{10000};
inline constexpr std::int64_t default_redirect_limit{5};

// NOLINTBEGIN(fuchsia-default-arguments-calls)
// NOLINTBEGIN(altera-struct-pack-align, readability-redundant-member-init)
struct proxy_route {
    proxy_mode mode{proxy_mode::system};
    std::string host{};
    std::int64_t port{};
    std::string username{};
    std::string password{};
};

struct fetch_request {
    std::string url{};
    std::string user_agent{"clash.meta/v" + std::string{node_model::kernel_version}};
    // Explicit order, e.g. {direct, http(local mixed port)} or {socks5, direct}.
    // A proxy-only list never silently falls back to direct.
    std::vector<proxy_route> routes{{}};
    // Custom headers and conditional validators are confined to the initial
    // origin; standard transport headers are managed by the HTTP backend.
    std::vector<http_header> headers{};
    std::string if_none_match{};
    std::string if_modified_since{};
    std::chrono::milliseconds total_timeout{default_total_timeout};
    std::chrono::milliseconds route_timeout{default_route_timeout};
    std::chrono::milliseconds idle_timeout{default_idle_timeout};
    std::int64_t max_redirects{default_redirect_limit};
    std::int64_t max_body_bytes{node_input::default_input_bytes};
    header_limits response_header_limits{};
    node_input::parse_limits parser_limits{};
    // Optional enterprise/private CA certificates in PEM. System trust is kept.
    std::string extra_ca_pem{};
};

// NOLINTEND(altera-struct-pack-align, readability-redundant-member-init)
// NOLINTEND(fuchsia-default-arguments-calls)

// Requires a live QCoreApplication/QGuiApplication, but exposes no Qt types.
// Blocking API for a worker. No file/profile state is changed on failure.
// NOLINTBEGIN(fuchsia-default-arguments-declarations)
[[nodiscard]] auto fetch_subscription(const fetch_request& request,
                                      std::stop_token cancellation = {}) noexcept -> fetch_result;
// Owns the request in a std::async worker. Scheduling may throw std::system_error
// or std::bad_alloc; network/parse failures are returned in fetch_result.
// Keep the future alive: destruction of a running std::async future may wait.
[[nodiscard]] auto fetch_subscription_async(fetch_request request,
                                            std::stop_token cancellation = {})
    -> std::future<fetch_result>;
// NOLINTEND(fuchsia-default-arguments-declarations)

} // namespace subscription_fetcher

#endif
