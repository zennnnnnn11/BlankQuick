#ifndef BLANK_QUICK_SERVICE_CONFIG_HPP
#define BLANK_QUICK_SERVICE_CONFIG_HPP

#include <QJsonObject>
#include <QString>

struct ServiceConfig {
    QJsonObject value;
    QString error;
};

[[nodiscard]] ServiceConfig compileServiceConfig(const QString& json, quint16 mixedPort,
                                                 const QString& device, bool tunEnabled = false,
                                                 const QString& providerRoot = {});
[[nodiscard]] ServiceConfig parseProfileConfig(const QString& json);
[[nodiscard]] QJsonObject serviceTunConfig(const QString& device, bool enabled);

#endif
