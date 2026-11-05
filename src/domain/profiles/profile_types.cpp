#include <blank/profiles/profile_types.hpp>
#include <cstdint>
#include <limits>
#include <optional>

namespace profile_storage {
auto used_bytes(const profile_record& profile) noexcept -> std::optional<std::int64_t> {
    const auto& account = profile.account;
    if (!account.upload_bytes || !account.download_bytes || *account.upload_bytes < 0 ||
        *account.download_bytes < 0 ||
        *account.upload_bytes >
            std::numeric_limits<std::int64_t>::max() - *account.download_bytes) {
        return std::nullopt;
    }
    return *account.upload_bytes + *account.download_bytes;
}

} // namespace profile_storage
