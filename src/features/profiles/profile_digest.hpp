#ifndef BLANK_QUICK_PROFILE_DIGEST_HPP // NOLINT(llvm-header-guard)
#define BLANK_QUICK_PROFILE_DIGEST_HPP

#include <string>
#include <string_view>

namespace profile_storage::detail {
[[nodiscard]] auto sha256(std::string_view data) -> std::string;
}

#endif
