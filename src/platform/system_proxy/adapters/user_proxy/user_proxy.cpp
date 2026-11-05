#include <Windows.h>

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <WinInet.h>
#include <array>
#include <blank/user_proxy/user_proxy.hpp>
#include <blank/windows/handle.hpp>
#include <cmath>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace {
void checkWinInet(BOOL success) {
    if (success == FALSE) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category());
    }
}

wchar_t* optionText(INTERNET_PER_CONN_OPTIONW option) {
    return option.Value.pszValue; // NOLINT(cppcoreguidelines-pro-type-union-access)
}

DWORD optionFlags(INTERNET_PER_CONN_OPTIONW option) {
    return option.Value.dwValue; // NOLINT(cppcoreguidelines-pro-type-union-access)
}

void setOptionFlags(INTERNET_PER_CONN_OPTIONW& option, quint32 flags) {
    option.dwOption = INTERNET_PER_CONN_FLAGS;
    option.Value.dwValue = flags; // NOLINT(cppcoreguidelines-pro-type-union-access)
}

void setOptionText(INTERNET_PER_CONN_OPTIONW& option, DWORD kind, std::wstring& text) {
    option.dwOption = kind;
    option.Value.pszValue = text.data(); // NOLINT(cppcoreguidelines-pro-type-union-access)
}

class QueryOptions final {
  public:
    std::array<INTERNET_PER_CONN_OPTIONW, 4> options{};

    QueryOptions() {
        options.at(0).dwOption = INTERNET_PER_CONN_FLAGS_UI;
        options.at(1).dwOption = INTERNET_PER_CONN_PROXY_SERVER;
        options.at(2).dwOption = INTERNET_PER_CONN_PROXY_BYPASS;
        options.at(3).dwOption = INTERNET_PER_CONN_AUTOCONFIG_URL;
    }

    ~QueryOptions() {
        for (const auto& option : std::span(options).subspan(1)) {
            if (auto* const text = optionText(option); text != nullptr) {
                GlobalFree(text);
            }
        }
    }

    QueryOptions(const QueryOptions&) = delete;
    QueryOptions& operator=(const QueryOptions&) = delete;
    QueryOptions(QueryOptions&&) = delete;
    QueryOptions& operator=(QueryOptions&&) = delete;
};

QString optionString(INTERNET_PER_CONN_OPTIONW option) {
    const auto* const text = optionText(option);
    if (text == nullptr) {
        return {};
    }
    constexpr std::size_t limit = 32768;
    const auto length = wcsnlen_s(text, limit + 1);
    if (length > limit) {
        throw std::runtime_error("WinINet returned an oversized setting.");
    }
    return QString::fromWCharArray(text, static_cast<qsizetype>(length));
}

QJsonObject encodeSnapshot(const UserProxySnapshot& snapshot) {
    return {
        {QStringLiteral("flags"), static_cast<qint64>(snapshot.flags)},
        {QStringLiteral("server"), snapshot.server},
        {QStringLiteral("bypass"), snapshot.bypass},
        {QStringLiteral("pac"), snapshot.pac},
    };
}

UserProxySnapshot decodeSnapshot(const QJsonObject& object) {
    const auto flags = object.value(QStringLiteral("flags")).toDouble(-1);
    if (!std::isfinite(flags) || flags < 0 || flags > std::numeric_limits<quint32>::max() ||
        std::floor(flags) != flags) {
        throw std::runtime_error("Invalid proxy recovery flags.");
    }
    const auto text = [&object](const QString& name) {
        const auto value = object.value(name);
        const auto string = value.toString();
        if (!value.isString() || string.size() > 32768 || string.contains(QChar::Null)) {
            throw std::runtime_error("Invalid proxy recovery string.");
        }
        return string;
    };
    return {
        .flags = static_cast<quint32>(flags),
        .server = text(QStringLiteral("server")),
        .bypass = text(QStringLiteral("bypass")),
        .pac = text(QStringLiteral("pac")),
    };
}

UserProxySnapshot intendedProxy(quint16 port) {
    return {
        .flags = PROXY_TYPE_DIRECT | PROXY_TYPE_PROXY,
        .server = QStringLiteral("127.0.0.1:%1").arg(port),
        .bypass = QStringLiteral("<local>"),
        .pac = {},
    };
}
} // namespace

UserProxySnapshot readUserProxy() {
    QueryOptions query;
    INTERNET_PER_CONN_OPTION_LISTW list{};
    list.dwSize = sizeof(list);
    list.dwOptionCount = static_cast<DWORD>(query.options.size());
    list.pOptions = query.options.data();
    DWORD size = sizeof(list);
    checkWinInet(
        InternetQueryOptionW(nullptr, INTERNET_OPTION_PER_CONNECTION_OPTION, &list, &size));
    return {
        .flags = static_cast<quint32>(optionFlags(query.options.at(0))),
        .server = optionString(query.options.at(1)),
        .bypass = optionString(query.options.at(2)),
        .pac = optionString(query.options.at(3)),
    };
}

void writeUserProxy(const UserProxySnapshot& snapshot) {
    std::array<INTERNET_PER_CONN_OPTIONW, 4> options{};
    auto server = snapshot.server.toStdWString();
    auto bypass = snapshot.bypass.toStdWString();
    auto pac = snapshot.pac.toStdWString();
    setOptionFlags(options.at(0), snapshot.flags);
    setOptionText(options.at(1), INTERNET_PER_CONN_PROXY_SERVER, server);
    setOptionText(options.at(2), INTERNET_PER_CONN_PROXY_BYPASS, bypass);
    setOptionText(options.at(3), INTERNET_PER_CONN_AUTOCONFIG_URL, pac);
    INTERNET_PER_CONN_OPTION_LISTW list{};
    list.dwSize = sizeof(list);
    list.dwOptionCount = static_cast<DWORD>(options.size());
    list.pOptions = options.data();
    checkWinInet(
        InternetSetOptionW(nullptr, INTERNET_OPTION_PER_CONNECTION_OPTION, &list, sizeof(list)));
    notifyUserProxy();
}

void notifyUserProxy() {

    const auto changed = InternetSetOptionW(nullptr, INTERNET_OPTION_SETTINGS_CHANGED, nullptr, 0);
    const auto changedError = GetLastError();
    const auto refreshed = InternetSetOptionW(nullptr, INTERNET_OPTION_REFRESH, nullptr, 0);
    checkWinInet(refreshed);
    if (changed == FALSE) {
        throw std::system_error(static_cast<int>(changedError), std::system_category());
    }
}

QString processBirth(quint32 processId) {
    const blank::windows::Handle process(
        OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId));
    checkWinInet(static_cast<BOOL>(process != nullptr));
    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    checkWinInet(GetProcessTimes(process.get(), &creation, &exit, &kernel, &user));
    return QString::number((quint64{creation.dwHighDateTime} << 32U) | creation.dwLowDateTime);
}

UserProxyTransaction::UserProxyTransaction(QString path, Reader read, Writer write)
    : path_(std::move(path)), read_(std::move(read)), write_(std::move(write)) {}

void UserProxyTransaction::apply(quint16 port) {
    // 恢复记录同时保存接管前快照和准备写入值，并在任何系统设置变更前提交。
    // 写入前再读一次现场，写入后再核验；失败保留记录，交给恢复流程判断归属。
    if (port == 0 || QFile::exists(path_)) {
        throw std::runtime_error("A proxy recovery record must be resolved before enabling.");
    }
    const auto before = read_();
    const auto intended = intendedProxy(port);
    const auto bytes = QJsonDocument(QJsonObject{
                                         {QStringLiteral("version"), 1},
                                         {QStringLiteral("before"), encodeSnapshot(before)},
                                         {QStringLiteral("intended"), encodeSnapshot(intended)},
                                     })
                           .toJson(QJsonDocument::Compact);
    QSaveFile journal(path_);
    journal.setDirectWriteFallback(false);

    if (!journal.open(QIODevice::WriteOnly) || journal.write(bytes) != bytes.size() ||
        !journal.commit()) {
        throw std::runtime_error("Cannot persist the proxy recovery snapshot; nothing changed.");
    }
    if (read_() != before) {
        throw std::runtime_error("Proxy settings changed before takeover.");
    }
    write_(intended);
    if (read_() != intended) {
        throw std::runtime_error("Proxy takeover verification failed; recovery record retained.");
    }
}

bool UserProxyTransaction::matches(quint16 port) const {
    return port != 0 && read_() == intendedProxy(port);
}

ProxyRestoreResult UserProxyTransaction::restore() {
    // 仅当现场仍等于“我们写入的值”或“原始快照”时恢复。第三种值说明用户或
    // 其他软件已经修改代理，返回 Conflict 并保留记录，不覆盖别人的新设置。
    QFile journal(path_);
    if (!journal.exists()) {
        return ProxyRestoreResult::NoRecord;
    }
    constexpr qint64 limit = qint64{2} * 1024 * 1024;
    if (!journal.open(QIODevice::ReadOnly) || journal.size() > limit) {
        throw std::runtime_error("Cannot read the proxy recovery record.");
    }
    const auto object = QJsonDocument::fromJson(journal.read(limit + 1)).object();
    journal.close();
    if (object.value(QStringLiteral("version")) != 1) {
        throw std::runtime_error("Invalid proxy recovery record version.");
    }
    const auto before = decodeSnapshot(object.value(QStringLiteral("before")).toObject());
    const auto intended = decodeSnapshot(object.value(QStringLiteral("intended")).toObject());
    const auto current = read_();
    if (current != before && current != intended) {
        return ProxyRestoreResult::Conflict;
    }

    write_(before);
    if (read_() != before) {
        throw std::runtime_error("Proxy restoration verification failed; record retained.");
    }
    if (!QFile::remove(path_)) {
        throw std::runtime_error("Proxy restored but recovery record could not be removed.");
    }
    return ProxyRestoreResult::Restored;
}
