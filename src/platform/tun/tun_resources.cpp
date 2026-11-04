#include <winsock2.h>
#include <ws2tcpip.h>

#include <windows.h>

#include <iphlpapi.h>

#include <Netioapi.h>
#include <Objbase.h>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>
#include <QUuid>
#include <SetupAPI.h>
#include <algorithm>
#include <array>
#include <blank/storage/file_durability.hpp>
#include <blank/tun/tun_resources.hpp>
#include <blank/windows/handle.hpp>
#include <cstring>
#include <devpropdef.h>
#include <span>
#include <stdexcept>
#include <system_error>

namespace {
constexpr GUID networkClass{
    .Data1 = 0x4d36e972,
    .Data2 = 0xe325,
    .Data3 = 0x11ce,
    .Data4 = {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18},
};
constexpr DEVPROPKEY presentProperty{
    .fmtid =
        {
            .Data1 = 0x540b947e,
            .Data2 = 0x8b40,
            .Data3 = 0x45bc,
            .Data4 = {0xa8, 0xa2, 0x6a, 0x0b, 0x89, 0x4c, 0xbd, 0xa2},
        },
    .pid = 5,
};

GUID deviceGuid(const QString& name) {
    // sing-tun 0.4.28 uses these raw MD5 bytes as a Windows GUID, not as a security digest.
    const auto bytes = QCryptographicHash::hash("wintun" + name.toUtf8(), QCryptographicHash::Md5);
    GUID guid{};
    std::memcpy(&guid, bytes.constData(), sizeof(guid));
    return guid;
}

TunPlan readPlan(const QString& journal) {
    QFile file(journal);
    if (!file.exists()) {
        return {};
    }
    if (!file.open(QIODevice::ReadOnly) || file.size() > 4096) {
        throw std::runtime_error("Cannot read the protected TUN journal.");
    }
    const auto object = QJsonDocument::fromJson(file.readAll()).object();
    const auto name = object.value(QStringLiteral("device")).toString();
    const auto guid = object.value(QStringLiteral("guid")).toString();
    if (object.value(QStringLiteral("version")) != 1 || name.size() != 43 ||
        !name.startsWith(QStringLiteral("BlankQuick-")) ||
        !std::ranges::all_of(name.sliced(11),
                             [](QChar value) {
                                 return (value >= u'0' && value <= u'9') ||
                                        (value >= u'a' && value <= u'f');
                             }) ||
        guid != tunGuid(name)) {
        throw std::runtime_error(
            "The protected TUN journal is invalid; no device will be removed.");
    }
    bool validLuid = true;
    const auto luidValue = object.value(QStringLiteral("luid")).toString();
    const auto luid = luidValue.isEmpty() ? 0 : luidValue.toULongLong(&validLuid, 16);
    if (!validLuid) {
        throw std::runtime_error("The TUN journal contains an invalid interface identity.");
    }
    return {
        .device = name,
        .guid = guid,
        .luid = luid,
        .error = {},
        .requirements =
            {
                .ipv6 = object.value(QStringLiteral("ipv6")).toBool(true),
                .defaultRoutes = object.value(QStringLiteral("default-routes")).toBool(true),
            },
    };
}

void writePlan(const QString& journal, const TunPlan& plan) {
    const auto bytes =
        QJsonDocument(QJsonObject{
                          {QStringLiteral("version"), 1},
                          {QStringLiteral("device"), plan.device},
                          {QStringLiteral("guid"), plan.guid},
                          {QStringLiteral("luid"), QString::number(plan.luid, 16)},
                          {QStringLiteral("ipv6"), plan.requirements.ipv6},
                          {QStringLiteral("default-routes"), plan.requirements.defaultRoutes},
                      })
            .toJson(QJsonDocument::Compact);
    const auto saved = writeDurableFile(journal, bytes);
    if (!saved.error.isEmpty()) {
        throw std::runtime_error(saved.error.toStdString());
    }
}

QString property(HDEVINFO devices, SP_DEVINFO_DATA& info, DWORD kind, DWORD expectedType) {
    std::array<wchar_t, 1024> buffer{};
    DWORD type = 0;
    DWORD size = 0;
    // SetupAPI's byte buffer carries a bounded UTF-16 Windows property.
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
    blank::windows::check(SetupDiGetDeviceRegistryPropertyW(devices, &info, kind, &type,
                                                            reinterpret_cast<BYTE*>(buffer.data()),
                                                            sizeof(buffer), &size) != FALSE);
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
    if (type != expectedType || size < sizeof(wchar_t) || size > sizeof(buffer) ||
        size % sizeof(wchar_t) != 0) {
        throw std::runtime_error("Unexpected Wintun device property encoding.");
    }
    auto result =
        QString::fromWCharArray(buffer.data(), static_cast<qsizetype>(size / sizeof(wchar_t)));
    while (result.endsWith(QChar::Null)) {
        result.chop(1);
    }
    return result;
}

QString registryText(HKEY key, const wchar_t* name) {
    std::array<wchar_t, 128> value{};
    DWORD size = sizeof(value);
    if (RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ, nullptr, value.data(), &size) !=
            ERROR_SUCCESS ||
        size < sizeof(wchar_t) || size > sizeof(value) || value.back() != L'\0') {
        throw std::runtime_error("Wintun driver binding is not published or has changed.");
    }
    return QString::fromWCharArray(value.data());
}

bool deviceExists(const TunPlan& plan, bool remove) {
    const auto destroy = [](void* value) { SetupDiDestroyDeviceInfoList(value); };
    const std::unique_ptr<void, decltype(destroy)> devices(
        SetupDiCreateDeviceInfoList(&networkClass, nullptr), destroy);
    blank::windows::check(devices.get() != INVALID_HANDLE_VALUE);
    SP_DEVINFO_DATA info{};
    info.cbSize = sizeof(info);
    const auto instance = (QStringLiteral("SWD\\WINTUN\\") + plan.guid).toStdWString();
    if (SetupDiOpenDeviceInfoW(devices.get(), instance.c_str(), nullptr, 0, &info) == FALSE) {
        if (GetLastError() == ERROR_NO_SUCH_DEVINST) {
            return false;
        }
        blank::windows::check(false);
    }
    if (info.ClassGuid != networkClass ||
        property(devices.get(), info, SPDRP_HARDWAREID, REG_MULTI_SZ)
                .compare(QStringLiteral("Wintun"), Qt::CaseInsensitive) != 0) {
        throw std::runtime_error("The recorded device is not an exact Wintun network adapter.");
    }
    const auto closeRegistry = [](HKEY value) { RegCloseKey(value); };
    const std::unique_ptr<HKEY__, decltype(closeRegistry)> driver(
        SetupDiOpenDevRegKey(devices.get(), &info, DICS_FLAG_GLOBAL, 0, DIREG_DRV, KEY_QUERY_VALUE),
        closeRegistry);
    blank::windows::check(driver.get() != INVALID_HANDLE_VALUE);
    if (QUuid(registryText(driver.get(), L"NetCfgInstanceId")) != QUuid(plan.guid) ||
        registryText(driver.get(), L"ComponentId")
                .compare(QStringLiteral("Wintun"), Qt::CaseInsensitive) != 0) {
        throw std::runtime_error("The Wintun driver's GUID or component identity does not match.");
    }
    DEVPROPTYPE type = 0;
    BYTE present = 0;
    DWORD size = 0;
    blank::windows::check(SetupDiGetDevicePropertyW(devices.get(), &info, &presentProperty, &type,
                                                    &present, sizeof(present), &size, 0) != FALSE);
    if (type != DEVPROP_TYPE_BOOLEAN || size != sizeof(present)) {
        throw std::runtime_error("Unexpected Wintun presence property.");
    }
    if (remove && present == 0) {
        SP_REMOVEDEVICE_PARAMS parameters{};
        parameters.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
        parameters.ClassInstallHeader.InstallFunction = DIF_REMOVE;
        parameters.Scope = DI_REMOVEDEVICE_GLOBAL;
        blank::windows::check(SetupDiSetClassInstallParamsW(devices.get(), &info,
                                                            &parameters.ClassInstallHeader,
                                                            sizeof(parameters)) != FALSE);
        blank::windows::check(SetupDiCallClassInstaller(DIF_REMOVE, devices.get(), &info) != FALSE);
    }
    return true;
}

bool routesRemain(const TunPlan& plan, bool requireDefaults = false) {
    MIB_IPFORWARD_TABLE2* raw = nullptr;
    const auto status = GetIpForwardTable2(AF_UNSPEC, &raw);
    if (status != NO_ERROR) {
        throw std::system_error(static_cast<int>(status), std::system_category());
    }
    const auto freeTable = [](MIB_IPFORWARD_TABLE2* table) { FreeMibTable(table); };
    const std::unique_ptr<MIB_IPFORWARD_TABLE2, decltype(freeTable)> table(raw, freeTable);
    const auto expected = deviceGuid(plan.device);
    bool ipv4 = false;
    bool ipv6 = false;
    for (const auto& route : std::span(raw->Table, raw->NumEntries)) {
        GUID actual{};
        if ((plan.luid != 0 && route.InterfaceLuid.Value == plan.luid) ||
            (ConvertInterfaceLuidToGuid(&route.InterfaceLuid, &actual) == NO_ERROR &&
             actual == expected)) {
            if (!requireDefaults) {
                return true;
            }
            if (route.DestinationPrefix.PrefixLength == 0) {
                // SOCKADDR_INET is the SDK IPv4/IPv6 union accepted by Qt's sockaddr overload.
                // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
                const QHostAddress address(
                    reinterpret_cast<const sockaddr*>(&route.DestinationPrefix.Prefix));
                // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
                ipv4 = ipv4 || address.protocol() == QAbstractSocket::IPv4Protocol;
                ipv6 = ipv6 || address.protocol() == QAbstractSocket::IPv6Protocol;
            }
        }
    }
    return ipv4 && (!plan.requirements.ipv6 || ipv6);
}
} // namespace

QString tunGuid(const QString& device) {
    const auto guid = deviceGuid(device);
    std::array<wchar_t, 40> text{};
    if (StringFromGUID2(guid, text.data(), static_cast<int>(text.size())) == 0) {
        throw std::runtime_error("Cannot encode the TUN device identity.");
    }
    return QString::fromWCharArray(text.data()).toLower();
}

TunPlan prepareTun(const QString& journal, TunRequirements requirements) try {
    if (!readPlan(journal).device.isEmpty()) {
        throw std::runtime_error("Previous TUN resources have not been released.");
    }
    auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    id.remove(u'-');
    TunPlan plan{.device = QStringLiteral("BlankQuick-") + id, .guid = {}, .luid = 0, .error = {}};
    plan.requirements = requirements;
    plan.guid = tunGuid(plan.device);
    if (deviceExists(plan, false) || routesRemain(plan)) {
        throw std::runtime_error("The planned TUN identity is already in use.");
    }
    writePlan(journal, plan);
    return plan;
} catch (const std::exception& error) {
    return {.device = {}, .guid = {}, .luid = 0, .error = QString::fromUtf8(error.what())};
}

TunPlan observeTun(const QString& journal, std::optional<TunRequirements> requirements) try {
    auto plan = readPlan(journal);
    if (requirements) {
        plan.requirements = *requirements;
    }
    if (plan.device.isEmpty() || !deviceExists(plan, false)) {
        throw std::runtime_error("The planned TUN device has not appeared.");
    }
    const auto guid = deviceGuid(plan.device);
    NET_LUID luid{};
    if (ConvertInterfaceGuidToLuid(&guid, &luid) != NO_ERROR) {
        throw std::runtime_error("The TUN network interface is not ready.");
    }
    // GetIfEntry2 fills the output enums before they are read; Windows expects a zeroed buffer.
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization)
    MIB_IF_ROW2 row{};
    row.InterfaceLuid = luid;
    if (GetIfEntry2(&row) != NO_ERROR || row.OperStatus != IfOperStatusUp ||
        (plan.requirements.defaultRoutes && !routesRemain(plan, true))) {
        throw std::runtime_error("The TUN interface or routes are not ready.");
    }
    plan.luid = luid.Value;
    writePlan(journal, plan);
    return plan;
} catch (const std::exception& error) {
    return {.device = {}, .guid = {}, .luid = 0, .error = QString::fromUtf8(error.what())};
}

TunHealth inspectTun(const QString& journal) try {
    const auto plan = readPlan(journal);
    if (plan.device.isEmpty()) {
        return {
            .healthy = false,
            .repairable = false,
            .error = QStringLiteral("The active TUN has no protected journal."),
        };
    }
    if (!deviceExists(plan, false)) {
        return {
            .healthy = false,
            .repairable = true,
            .error = QStringLiteral("The owned TUN device is absent."),
        };
    }
    NET_LUID luid{};
    const auto guid = deviceGuid(plan.device);
    const auto status = ConvertInterfaceGuidToLuid(&guid, &luid);
    if (status == ERROR_FILE_NOT_FOUND || status == ERROR_NOT_FOUND) {
        return {
            .healthy = false,
            .repairable = true,
            .error = QStringLiteral("The owned TUN interface is absent."),
        };
    }
    if (status != NO_ERROR) {
        throw std::system_error(static_cast<int>(status), std::system_category());
    }
    // GetIfEntry2 overwrites the SDK output enums before use.
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization)
    MIB_IF_ROW2 row{};
    row.InterfaceLuid = luid;
    const auto rowStatus = GetIfEntry2(&row);
    if (rowStatus != NO_ERROR) {
        throw std::system_error(static_cast<int>(rowStatus), std::system_category());
    }
    if (plan.luid != luid.Value || row.OperStatus != IfOperStatusUp ||
        (plan.requirements.defaultRoutes && !routesRemain(plan, true))) {
        return {
            .healthy = false,
            .repairable = true,
            .error = QStringLiteral("The owned TUN interface or default routes are not ready."),
        };
    }
    return {.healthy = true, .repairable = false, .error = {}};
} catch (const std::exception& error) {
    return {.healthy = false, .repairable = false, .error = QString::fromUtf8(error.what())};
}

QString releaseTun(const QString& journal) try {
    const auto plan = readPlan(journal);
    if (plan.device.isEmpty()) {
        return {};
    }
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < 12000) {
        const bool device = deviceExists(plan, true);
        if (!device && !routesRemain(plan)) {
            if (!QFile::remove(journal)) {
                throw std::runtime_error(
                    "TUN resources were released but the journal could not be cleared.");
            }
            return flushDirectory(QFileInfo(journal).absolutePath());
        }
        QThread::msleep(100);
    }
    return QStringLiteral("TUN resources are still present; the protected journal was retained.");
} catch (const std::exception& error) {
    return QString::fromUtf8(error.what());
}
