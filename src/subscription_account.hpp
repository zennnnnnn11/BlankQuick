#ifndef BLANK_QUICK_SUBSCRIPTION_ACCOUNT_HPP // NOLINT(llvm-header-guard)
#define BLANK_QUICK_SUBSCRIPTION_ACCOUNT_HPP

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Account values and diagnostics require no node parser or network implementation.
namespace subscription_data {
// NOLINTBEGIN(altera-struct-pack-align,readability-redundant-member-init)
struct account_metadata {
    // Bytes and Unix seconds. Missing differs from explicit zero; no unlimited
    // quota convention is inferred. expire=0 means no expiry on common panels.
    std::optional<std::int64_t> upload_bytes{};
    std::optional<std::int64_t> download_bytes{};
    std::optional<std::int64_t> total_bytes{};
    std::optional<std::int64_t> expire_unix_seconds{};
    std::optional<std::string> profile_name{};
    std::optional<std::string> suggested_filename{};
    std::optional<std::string> home_page_url{};
    std::optional<std::string> plan_name{};
    std::optional<std::chrono::seconds> update_interval{};
    std::optional<std::string> etag{};
    std::optional<std::string> last_modified{};
};

enum class metadata_field : std::int8_t {
    userinfo,
    upload,
    download,
    total,
    expire,
    title,
    disposition,
    home_page,
    interval,
    plan,
    etag,
    last_modified,
};

enum class metadata_problem : std::int8_t {
    malformed,
    duplicate,
    out_of_range,
    unsupported_encoding,
};

struct metadata_warning {
    metadata_field field{};
    metadata_problem problem{};
};

enum class metadata_error : std::int8_t {
    none,
    resource_limit,
    allocation_failure,
    unexpected_failure,
};

struct metadata_result {
    account_metadata account{};
    std::vector<metadata_warning> warnings{};
    metadata_error error{metadata_error::none};
};
// NOLINTEND(altera-struct-pack-align,readability-redundant-member-init)
} // namespace subscription_data

// Original names remain available to existing source callers.
namespace subscription_fetcher {
using subscription_data::account_metadata;
using subscription_data::metadata_error;
using subscription_data::metadata_field;
using subscription_data::metadata_problem;
using subscription_data::metadata_result;
using subscription_data::metadata_warning;
} // namespace subscription_fetcher

#endif
