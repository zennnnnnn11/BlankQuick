#include <winsock2.h>
#include <ws2tcpip.h>

#include <windows.h>

#include <iphlpapi.h>

#include <QCryptographicHash>
#include <QHash>
#include <QHostAddress>
#include <QJsonArray>
#include <QStringList>
#include <algorithm>
#include <blank/environment/network_inventory.hpp>
#include <cstddef>
#include <exception>
#include <memory>
#include <span>
#include <stdexcept>
#include <system_error>
#include <vector>

namespace {
void checkStatus(ULONG status) {
    if (status != NO_ERROR) {
        throw std::system_error(static_cast<int>(status), std::system_category());
    }
}

bool ownedName(const wchar_t* name) {
    if (name == nullptr) {
        return false;
    }
    const auto text = QString::fromWCharArray(name);

    return text.size() == 43 && text.startsWith(QStringLiteral("BlankQuick-")) &&
           std::ranges::all_of(text.sliced(11), [](QChar value) {
               return (value >= u'0' && value <= u'9') || (value >= u'a' && value <= u'f');
           });
}

QString socketText(const void* address) {
    return QHostAddress(static_cast<const sockaddr*>(address)).toString();
}
NetworkSnapshot readNetworkInventory(bool detailed) try {
    ULONG bytes = 16384;
    std::vector<std::max_align_t> storage;
    ULONG status = ERROR_BUFFER_OVERFLOW;
    IP_ADAPTER_ADDRESSES* adapters = nullptr;
    for (int attempt = 0; attempt < 3 && status == ERROR_BUFFER_OVERFLOW; ++attempt) {
        if (bytes > 16 * 1024 * 1024) {
            throw std::runtime_error("Network inventory exceeded its size limit.");
        }
        storage.resize((bytes + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t));

        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
        status = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST,
                                      nullptr, adapters, &bytes);
    }
    if (status == ERROR_NO_DATA) {
        return {.fingerprint = "offline", .online = false, .error = {}};
    }
    checkStatus(status);
    QStringList entries;
    QJsonArray adapterDetails;
    QJsonArray routeDetails;
    QHash<quint64, bool> connected;
    for (const auto* adapter = adapters; adapter != nullptr; adapter = adapter->Next) {
        if (detailed) {
            QJsonArray addresses;
            QJsonArray dnsServers;
            for (const auto* address = adapter->FirstUnicastAddress; address != nullptr;
                 address = address->Next) {
                addresses.append(socketText(address->Address.lpSockaddr) + u'/' +
                                 QString::number(address->OnLinkPrefixLength));
            }
            for (const auto* dns = adapter->FirstDnsServerAddress; dns != nullptr;
                 dns = dns->Next) {
                dnsServers.append(socketText(dns->Address.lpSockaddr));
            }
            adapterDetails.append(QJsonObject{
                {QStringLiteral("name"), QString::fromWCharArray(adapter->FriendlyName)},
                {QStringLiteral("luid"), QString::number(adapter->Luid.Value)},
                {QStringLiteral("status"), static_cast<int>(adapter->OperStatus)},
                {QStringLiteral("mtu"), static_cast<qint64>(adapter->Mtu)},
                {QStringLiteral("addresses"), addresses},
                {QStringLiteral("dns"), dnsServers},
            });
        }
        if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK || ownedName(adapter->FriendlyName)) {
            continue;
        }
        const auto id = QString::number(adapter->Luid.Value);
        entries.append(id + u':' + QString::number(adapter->OperStatus) + u':' +
                       QString::number(adapter->Mtu));
        connected.insert(adapter->Luid.Value, adapter->OperStatus == IfOperStatusUp);
        for (auto const* address = adapter->FirstUnicastAddress; address != nullptr;
             address = address->Next) {
            entries.append(id + QStringLiteral(":ip:") + socketText(address->Address.lpSockaddr) +
                           u'/' + QString::number(address->OnLinkPrefixLength));
        }
        int priority = 0;
        for (auto const* dns = adapter->FirstDnsServerAddress; dns != nullptr; dns = dns->Next) {
            entries.append(id + QStringLiteral(":dns:") + QString::number(priority++) + u':' +
                           socketText(dns->Address.lpSockaddr));
        }
    }
    MIB_IPINTERFACE_TABLE* ipRaw = nullptr;
    checkStatus(GetIpInterfaceTable(AF_UNSPEC, &ipRaw));
    const auto freeIp = [](MIB_IPINTERFACE_TABLE* table) { FreeMibTable(table); };
    const std::unique_ptr<MIB_IPINTERFACE_TABLE, decltype(freeIp)> ipTable(ipRaw, freeIp);
    for (const auto& row : std::span(ipRaw->Table, ipRaw->NumEntries)) {
        if (connected.contains(row.InterfaceLuid.Value)) {
            entries.append(QString::number(row.InterfaceLuid.Value) + QStringLiteral(":binding:") +
                           QString::number(row.Family) + u':' + QString::number(row.Metric) + u':' +
                           QString::number(row.NlMtu) + u':' + QString::number(row.Connected));
        }
    }
    MIB_IPFORWARD_TABLE2* raw = nullptr;
    checkStatus(GetIpForwardTable2(AF_UNSPEC, &raw));
    const auto freeTable = [](MIB_IPFORWARD_TABLE2* table) { FreeMibTable(table); };
    const std::unique_ptr<MIB_IPFORWARD_TABLE2, decltype(freeTable)> table(raw, freeTable);
    bool online = false;
    for (const auto& route : std::span(raw->Table, raw->NumEntries)) {
        if (detailed) {
            routeDetails.append(QJsonObject{
                {QStringLiteral("luid"), QString::number(route.InterfaceLuid.Value)},
                {
                    QStringLiteral("destination"),
                    socketText(&route.DestinationPrefix.Prefix) + u'/' +
                        QString::number(route.DestinationPrefix.PrefixLength),
                },
                {QStringLiteral("gateway"), socketText(&route.NextHop)},
                {QStringLiteral("metric"), static_cast<qint64>(route.Metric)},
            });
        }
        if (route.DestinationPrefix.PrefixLength != 0 ||
            !connected.value(route.InterfaceLuid.Value, false)) {
            continue;
        }
        online = true;
        entries.append(QString::number(route.InterfaceLuid.Value) + QStringLiteral(":default:") +
                       socketText(&route.DestinationPrefix.Prefix) + u':' +
                       socketText(&route.NextHop) + u':' + QString::number(route.Metric));
    }
    entries.sort();
    return {
        .fingerprint =
            QCryptographicHash::hash(entries.join(u'\n').toUtf8(), QCryptographicHash::Sha256),
        .online = online,
        .error = {},
        .details =
            {
                {QStringLiteral("adapters"), adapterDetails},
                {QStringLiteral("routes"), routeDetails},
            },
    };
} catch (const std::exception& error) {
    return {.fingerprint = {}, .online = false, .error = QString::fromUtf8(error.what())};
}

} // namespace

NetworkSnapshot readNetworkSnapshot() {
    // 常规观察只返回用于比较的指纹和在线事实；诊断入口另外附带完整明细。
    // 指纹排除本客户端拥有的虚拟网卡，避免自己的 TUN 变化触发无休止自我修复。
    return readNetworkInventory(false);
}

NetworkSnapshot readNetworkDiagnostics() {
    return readNetworkInventory(true);
}
