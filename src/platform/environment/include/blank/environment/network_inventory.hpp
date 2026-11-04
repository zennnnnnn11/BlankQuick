#ifndef BLANK_QUICK_NETWORK_INVENTORY_HPP
#define BLANK_QUICK_NETWORK_INVENTORY_HPP

#include <QByteArray>
#include <QJsonObject>
#include <QString>

struct NetworkSnapshot {
    QByteArray fingerprint;
    bool online = false;
    QString error;
    QJsonObject details = {};
};

[[nodiscard]] NetworkSnapshot readNetworkSnapshot();
[[nodiscard]] NetworkSnapshot readNetworkDiagnostics();

#endif
