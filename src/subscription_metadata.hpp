#ifndef BLANK_QUICK_SUBSCRIPTION_METADATA_HPP // NOLINT(llvm-header-guard)
#define BLANK_QUICK_SUBSCRIPTION_METADATA_HPP

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "subscription_account.hpp"

namespace subscription_fetcher {

inline constexpr std::int64_t default_header_fields{256};
inline constexpr std::int64_t default_header_bytes{65536};
inline constexpr std::int64_t default_header_value_bytes{8192};

// Values are owned UTF-8/HTTP bytes, never views into a destroyed network reply.
// NOLINTBEGIN(altera-struct-pack-align, readability-redundant-member-init)
struct http_header {
    std::string name{};
    std::string value{};
};

struct header_limits {
    std::int64_t max_fields{default_header_fields};
    std::int64_t max_bytes{default_header_bytes};
    std::int64_t max_value_bytes{default_header_value_bytes};
};

// NOLINTEND(altera-struct-pack-align, readability-redundant-member-init)

// Exact header names win over cloud-storage metadata prefixes. Invalid optional
// fields do not invalidate valid nodes. Warnings never contain header values.
// NOLINTBEGIN(fuchsia-default-arguments-declarations)
[[nodiscard]] auto parse_response_headers(std::span<const http_header> headers,
                                          const header_limits& limits = {}) noexcept
    -> metadata_result;
// NOLINTEND(fuchsia-default-arguments-declarations)

} // namespace subscription_fetcher

#endif
