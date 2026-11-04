#ifndef BLANK_QUICK_TUN_RESOURCES_HPP
#define BLANK_QUICK_TUN_RESOURCES_HPP

#include <QString>
#include <optional>

struct TunRequirements
{
    bool ipv6 = true;
    bool defaultRoutes = true;
};

struct TunPlan
{
    QString device;
    QString guid;
    quint64 luid = 0;
    QString error;
    TunRequirements requirements{};
};

struct TunHealth
{
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
