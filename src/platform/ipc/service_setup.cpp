#include <Windows.h>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QThread>
#include <array>
#include <blank/ipc/service_security.hpp>
#include <blank/ipc/service_setup.hpp>
#include <exception>
#include <type_traits>

namespace {
struct ScmCloser {
    void operator()(SC_HANDLE value) const noexcept {
        CloseServiceHandle(value);
    }
};

using ScmHandle = std::unique_ptr<std::remove_pointer_t<SC_HANDLE>, ScmCloser>;

SERVICE_STATUS_PROCESS status(SC_HANDLE service) {
    SERVICE_STATUS_PROCESS result{};
    DWORD size = 0;

    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
    blank::windows::check(QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
                                               reinterpret_cast<BYTE*>(&result), sizeof(result),
                                               &size) != FALSE);
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
    return result;
}

QStringList bundleFiles() {
    QStringList files{
        QStringLiteral("blank_quick_service.exe"), QStringLiteral("kernel_signal.exe"),
        QStringLiteral("core/mihomo.exe"),         QStringLiteral("core/wintun.dll"),
        QStringLiteral("WINTUN-LICENSE.txt"),      QStringLiteral("MIHOMO-LICENSE.txt"),
    };
    files.reserve(12);
#ifdef QT_DEBUG
    constexpr auto suffix = "d.dll";
#else
    constexpr auto suffix = ".dll";
#endif
    for (const auto* module : {"Core", "Network", "Concurrent", "WebSockets"}) {
        files.append(QStringLiteral("Qt6") + QString::fromLatin1(module) +
                     QString::fromLatin1(suffix));
    }
    files.append(QStringLiteral("tls/qschannelbackend") + QString::fromLatin1(suffix));
#ifdef __SANITIZE_ADDRESS__
    files.append(QStringLiteral("clang_rt.asan_dynamic-x86_64.dll"));
#endif
    return files;
}

void copyFile(const QString& source, const QString& destination) {
    if (QFileInfo(source).isSymLink() ||
        (QFileInfo::exists(destination) && QFileInfo(destination).isSymLink())) {
        throw std::runtime_error("Service deployment refuses reparse-point files.");
    }
    QFile input(source);
    QSaveFile output(destination);
    output.setDirectWriteFallback(false);
    if (!input.open(QIODevice::ReadOnly) || input.size() > qint64{128} * 1024 * 1024 ||
        !output.open(QIODevice::WriteOnly)) {
        throw std::runtime_error("Cannot open a service deployment artifact.");
    }
    while (!input.atEnd()) {
        const auto bytes = input.read(65536);
        if (input.error() != QFileDevice::NoError || bytes.isEmpty() ||
            output.write(bytes) != bytes.size()) {
            throw std::runtime_error("Cannot copy a service deployment artifact.");
        }
    }
    if (!output.commit()) {
        throw std::runtime_error("Cannot commit a service deployment artifact.");
    }
    verifyTrustedPath(destination);
}

void waitStopped(SC_HANDLE service) {
    const auto initial = status(service);
    if (initial.dwCurrentState == SERVICE_STOPPED) {
        return;
    }
    const blank::windows::Handle process(OpenProcess(SYNCHRONIZE, FALSE, initial.dwProcessId));
    blank::windows::check(process != nullptr);
    SERVICE_STATUS value{};
    if (ControlService(service, SERVICE_CONTROL_STOP, &value) == FALSE &&
        GetLastError() != ERROR_SERVICE_NOT_ACTIVE) {
        blank::windows::check(false);
    }
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < 30000) {
        const auto current = status(service);
        if (current.dwCurrentState == SERVICE_STOPPED) {
            if (current.dwWin32ExitCode != NO_ERROR) {
                throw std::runtime_error(
                    "Service cleanup failed; installation and recovery data were retained.");
            }
            if (WaitForSingleObject(process.get(), 5000) != WAIT_OBJECT_0) {
                throw std::runtime_error("SCM stopped, but the service process has not exited.");
            }
            return;
        }
        QThread::msleep(100);
    }
    throw std::runtime_error("Service did not finish cleanup before the uninstall deadline.");
}
} // namespace

ServiceInstallation inspectService(bool start) try {
    const ScmHandle manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    blank::windows::check(manager != nullptr);
    const ScmHandle service(OpenServiceW(manager.get(), serviceName,
                                         SERVICE_QUERY_STATUS | (start ? SERVICE_START : 0)));
    if (!service) {
        if (GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST) {
            return {};
        }
        blank::windows::check(false);
    }
    if (start && status(service.get()).dwCurrentState == SERVICE_STOPPED) {
        if (StartServiceW(service.get(), 0, nullptr) == FALSE &&
            GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) {
            blank::windows::check(false);
        }
    }
    QElapsedTimer elapsed;
    elapsed.start();
    auto current = status(service.get());
    while (start && current.dwCurrentState == SERVICE_START_PENDING && elapsed.elapsed() < 20000) {
        QThread::msleep(100);
        current = status(service.get());
    }
    return {
        .installed = true,
        .running = current.dwCurrentState == SERVICE_RUNNING,
        .error = current.dwCurrentState == SERVICE_RUNNING
                     ? QString{}
                     : QStringLiteral("服务尚未就绪，请重试或检查服务恢复记录"),
    };
} catch (const std::exception& error) {
    return {.installed = false, .running = false, .error = QString::fromUtf8(error.what())};
}

QString installService() try {
    if (!elevatedProcess()) {
        throw std::runtime_error("Service installation requires an elevated installer.");
    }
    const ScmHandle manager(
        OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE));
    blank::windows::check(manager != nullptr);
    const ScmHandle existing(OpenServiceW(manager.get(), serviceName, SERVICE_QUERY_STATUS));
    if (existing) {
        throw std::runtime_error(
            "Service already exists; uninstall it before replacing its trusted files.");
    }
    if (GetLastError() != ERROR_SERVICE_DOES_NOT_EXIST) {
        blank::windows::check(false);
    }
    const auto source = QCoreApplication::applicationDirPath();
    const auto destination = installedServiceDirectory();
    protectDirectory(destination, true);
    protectDirectory(destination + QStringLiteral("/core"), true);
    protectDirectory(destination + QStringLiteral("/tls"), true);
    protectDirectory(serviceDataDirectory(), false);
    const auto files = bundleFiles();
    for (const auto& name : files) {
        copyFile(source + u'/' + name, destination + u'/' + name);
    }
    const auto runtimes = QDir(source).entryList(
        {
            QStringLiteral("vcruntime140*.dll"),
            QStringLiteral("msvcp140*.dll"),
            QStringLiteral("concrt140*.dll"),
            QStringLiteral("ucrtbased.dll"),
        },
        QDir::Files);
    for (const auto& name : runtimes) {
        copyFile(source + u'/' + name, destination + u'/' + name);
    }
    const auto executable =
        (u'"' + QDir::toNativeSeparators(destination + QStringLiteral("/blank_quick_service.exe")) +
         u'"')
            .toStdWString();
    const ScmHandle service(CreateServiceW(
        manager.get(), serviceName, L"BlankQuick privileged TUN service", SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, executable.c_str(),
        nullptr, nullptr, nullptr, nullptr, nullptr));
    blank::windows::check(service != nullptr);
    const auto descriptor =
        securityDescriptor(QStringLiteral("D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;0x00020095;;;IU)"));
    blank::windows::check(SetServiceObjectSecurity(service.get(), DACL_SECURITY_INFORMATION,
                                                   descriptor.get()) != FALSE);
    std::array<SC_ACTION, 3> actions{
        {
            {.Type = SC_ACTION_RESTART, .Delay = 1000},
            {.Type = SC_ACTION_RESTART, .Delay = 3000},
            {.Type = SC_ACTION_RESTART, .Delay = 5000},
        },
    };
    SERVICE_FAILURE_ACTIONSW failures{};
    failures.dwResetPeriod = 86400;
    failures.cActions = static_cast<DWORD>(actions.size());
    failures.lpsaActions = actions.data();
    blank::windows::check(
        ChangeServiceConfig2W(service.get(), SERVICE_CONFIG_FAILURE_ACTIONS, &failures) != FALSE);
    SERVICE_FAILURE_ACTIONS_FLAG nonCrash{TRUE};
    blank::windows::check(ChangeServiceConfig2W(service.get(), SERVICE_CONFIG_FAILURE_ACTIONS_FLAG,
                                                &nonCrash) != FALSE);
    blank::windows::check(StartServiceW(service.get(), 0, nullptr) != FALSE);
    const auto ready = inspectService(true);
    if (!ready.running) {
        throw std::runtime_error(ready.error.toStdString());
    }
    return {};
} catch (const std::exception& error) {
    return QString::fromUtf8(error.what());
}

QString uninstallService() try {
    if (!elevatedProcess()) {
        throw std::runtime_error("Service removal requires an elevated installer.");
    }
    const ScmHandle manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    blank::windows::check(manager != nullptr);
    const ScmHandle service(
        OpenServiceW(manager.get(), serviceName, SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE));
    if (!service) {
        if (GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST) {
            return {};
        }
        blank::windows::check(false);
    }
    waitStopped(service.get());
    if (QFileInfo::exists(serviceDataDirectory() + QStringLiteral("/tun.json"))) {
        throw std::runtime_error(
            "TUN recovery is unresolved; the service and journal were retained.");
    }
    blank::windows::check(DeleteService(service.get()) != FALSE);

    const auto destination = installedServiceDirectory();
    verifyTrustedPath(destination);
    const auto files = bundleFiles();
    for (const auto& name : files) {
        const auto path = destination + u'/' + name;
        if (QFileInfo::exists(path)) {
            verifyTrustedPath(path);
            QFile::remove(path);
        }
    }
    QDir(destination).rmdir(QStringLiteral("core"));
    QDir(destination).rmdir(QStringLiteral("tls"));
    return {};
} catch (const std::exception& error) {
    return QString::fromUtf8(error.what());
}
