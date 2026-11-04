#ifndef BLANK_QUICK_TIMEOUTS_HPP // NOLINT(llvm-header-guard)
#define BLANK_QUICK_TIMEOUTS_HPP

#include <chrono>

namespace timeout_budget {
inline constexpr std::chrono::milliseconds request{10000};
inline constexpr std::chrono::milliseconds service_overhead{10000};
inline constexpr std::chrono::milliseconds ipc_overhead{10000};
inline constexpr std::chrono::milliseconds native_check{300000};
inline constexpr std::chrono::milliseconds config_apply{330000};
inline constexpr std::chrono::milliseconds service_verification{30000};
inline constexpr auto config_reply =
    config_apply + service_overhead + service_verification + ipc_overhead;
inline constexpr auto config_submission = config_reply + ipc_overhead;
inline constexpr std::chrono::milliseconds verification{180000};
inline constexpr std::chrono::milliseconds tun{18000};
inline constexpr auto tun_reply = tun + ipc_overhead;
inline constexpr std::chrono::milliseconds geo_update{150000};
inline constexpr std::chrono::milliseconds provider_update{45000};
inline constexpr std::chrono::milliseconds provider_read{120000};

static_assert(native_check < config_apply);
static_assert(config_apply < config_reply);
static_assert(config_reply < config_submission);
static_assert(provider_read < verification);
static_assert(tun < tun_reply);
} // namespace timeout_budget

#endif
