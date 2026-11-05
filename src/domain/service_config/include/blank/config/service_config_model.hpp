#ifndef BLANK_QUICK_SERVICE_CONFIG_MODEL_HPP
#define BLANK_QUICK_SERVICE_CONFIG_MODEL_HPP

#include <QJsonObject>
#include <QString>

struct ServiceConfig {
    QJsonObject value;
    QString error;
};

[[nodiscard]] ServiceConfig composeServiceConfig(const QString& json, quint16 mixedPort,
                                                 const QString& device, bool tunEnabled = false);
[[nodiscard]] ServiceConfig parseProfileConfig(const QString& json);
[[nodiscard]] QJsonObject serviceTunConfig(const QString& device, bool enabled);

#endif
