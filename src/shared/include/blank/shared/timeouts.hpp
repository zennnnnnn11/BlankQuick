#ifndef BLANK_QUICK_TIMEOUTS_HPP // NOLINT(llvm-header-guard)
#define BLANK_QUICK_TIMEOUTS_HPP

#include <chrono>

namespace timeout_budget {
// 从内向外分配时间：内核执行 < 服务核验与回复 < 管道等待 < 客户端事务等待。
// 外层若先结束，会把仍在执行的提交变成不确定结果。调整某个预算时必须同时
// 检查整条等待链；下方 static_assert 固定其中不可颠倒的大小关系。
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
