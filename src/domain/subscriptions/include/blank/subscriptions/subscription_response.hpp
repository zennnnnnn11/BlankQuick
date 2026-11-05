#ifndef BLANK_QUICK_SUBSCRIPTION_RESPONSE_HPP // NOLINT(llvm-header-guard)
#define BLANK_QUICK_SUBSCRIPTION_RESPONSE_HPP

#include <blank/nodes/node_input.hpp>
#include <blank/subscriptions/subscription_data.hpp>
#include <blank/subscriptions/subscription_metadata.hpp>
#include <cstdint>
#include <exception>
#include <string>
#include <vector>

namespace subscription_data {
// NOLINTBEGIN(altera-struct-pack-align,readability-redundant-member-init)
struct http_response {
    std::string body{};
    std::vector<subscription_fetcher::http_header> headers{};
    std::int64_t status{};
};

struct response_options {
    subscription_fetcher::header_limits headers{};
    node_input::parse_limits parser{};
};

class response_failure final : public std::exception {
  public:
    explicit response_failure(fetch_error selected) noexcept : error{selected} {}
    [[nodiscard]] auto code() const noexcept -> fetch_error {
        return error;
    }

  private:
    fetch_error error{};
};
// NOLINTEND(altera-struct-pack-align,readability-redundant-member-init)

// Interpret already-received bytes. No socket, clock, QCoreApplication or route
// selection is involved. Terminal metadata errors abort the entire fetch, as
// opposed to ordinary transport failures which may allow another route.
[[nodiscard]] auto decode_response(http_response&& response, const response_options& options)
    -> fetch_result;
} // namespace subscription_data

#endif
