#ifndef BLANK_QUICK_TUN_RESOURCES_HPP
#define BLANK_QUICK_TUN_RESOURCES_HPP

#include <QString>
#include <optional>

struct TunRequirements {
    bool ipv6 = true;
    bool defaultRoutes = true;
};

// device/GUID 是创建前写入恢复日志的计划身份，LUID 是设备出现后观察到的现场身份。
// 清理和健康检查依据这些身份定位资源，不能仅凭网卡名称前缀处理其他实例。
struct TunPlan {
    QString device;
    QString guid;
    quint64 luid = 0;
    QString error;
    TunRequirements requirements{};
};

// 不健康与可修复分开：系统查询失败属于无法确认现场，不能因此直接重建网卡。
// repairable 只说明已识别的所属资源缺失/不完整，是否重建仍由服务调度者决定。
struct TunHealth {
    bool healthy = false;
    bool repairable = false;
    QString error;
};

[[nodiscard]] QString tunGuid(const QString& device);
[[nodiscard]] TunPlan prepareTun(const QString& journal, TunRequirements requirements = {});
[[nodiscard]] TunPlan observeTun(const QString& journal,
                                 std::optional<TunRequirements> requirements = std::nullopt);
[[nodiscard]] TunHealth inspectTun(const QString& journal);
[[nodiscard]] QString releaseTun(const QString& journal);

#endif
