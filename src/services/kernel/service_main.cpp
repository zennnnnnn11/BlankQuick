#include <QCoreApplication>
#include <QFile>
#include <QSaveFile>
#include <array>
#include <blank/ipc/service_setup.hpp>
#include <blank/qt/child.hpp>
#include <chrono>
#include <exception>
#include <thread>

#include "service_host.hpp"

namespace {
void serviceMessage(QtMsgType type, const QMessageLogContext& context, const QString& message) {
    const auto text = qFormatLogMessage(type, context, message);
    const auto wide = text.toStdWString();
    OutputDebugStringW(wide.c_str());
    if (type != QtWarningMsg && type != QtCriticalMsg && type != QtFatalMsg) {
        return;
    }
    try {
        const auto root = serviceDataDirectory();
        verifyTrustedPath(root);
        QSaveFile record(root + QStringLiteral("/runtime-error.txt"));
        record.setDirectWriteFallback(false);
        const auto bytes = text.left(4096).toUtf8();
        if (record.open(QIODevice::WriteOnly) && record.write(bytes) == bytes.size()) {
            static_cast<void>(record.commit());
        }
    } catch (...) {
        OutputDebugStringW(L"BlankQuick could not write its protected runtime error.\n");
    }
}

struct ServiceContext {
    blank::windows::Handle stop{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    blank::windows::Handle done{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    SERVICE_STATUS_HANDLE handle = nullptr;
    SERVICE_STATUS status{};
};

void finishService(const std::shared_ptr<ServiceContext>& context, const QString& error) {
    auto& status = context->status;
    status.dwCurrentState = SERVICE_STOPPED;
    status.dwControlsAccepted = 0;
    status.dwWaitHint = 0;
    status.dwCheckPoint = 0;
    if (!error.isEmpty()) {
        status.dwWin32ExitCode = ERROR_SERVICE_SPECIFIC_ERROR;
        status.dwServiceSpecificExitCode = 1;
        auto* const source = RegisterEventSourceW(nullptr, serviceName);
        if (source != nullptr) {
            const auto text = error.left(1024).toStdWString();
            std::array<const wchar_t*, 1> messages{text.c_str()};
            ReportEventW(source, EVENTLOG_ERROR_TYPE, 0, 1, nullptr, 1, 0, messages.data(),
                         nullptr);
            DeregisterEventSource(source);
        }
    }
    SetServiceStatus(context->handle, &status);
    SetEvent(context->done.get());
    QCoreApplication::exit(error.isEmpty() ? 0 : 1);
}

DWORD WINAPI serviceControl(DWORD control, DWORD /*eventType*/, void* /*eventData*/, void* data) {
    // SCM 控制回调不在 Qt 会话线程，只置位停止事件。状态更新和异步清理由
    // QWinEventNotifier 转回事件循环执行，避免从系统回调直接操作 ServiceHost。
    if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN ||
        control == SERVICE_CONTROL_PRESHUTDOWN) {
        const auto* context = static_cast<ServiceContext*>(data);
        return SetEvent(context->stop.get()) != FALSE ? NO_ERROR : GetLastError();
    }
    return control == SERVICE_CONTROL_INTERROGATE ? NO_ERROR : ERROR_CALL_NOT_IMPLEMENTED;
}

void startRuntime(const std::shared_ptr<ServiceContext>& context) try {
    // SERVICE_RUNNING 只在启动恢复完成、服务管道就绪后发布；STOPPED 只在
    // drained 或明确致命失败后发布。等待期间持续更新 SCM checkpoint，
    // 不能在收到停止命令时立即退出，绕过内核和 TUN 的清理屏障。
    auto* application = QCoreApplication::instance();

    auto* host = &blank::qt::makeChild<ServiceHost>(*application);
    auto* notify = &blank::qt::makeChild<QWinEventNotifier>(*host, context->stop.get());
    auto* pending = &blank::qt::makeChild<QTimer>(*host);
    pending->setInterval(std::chrono::milliseconds{2000});
    QObject::connect(pending, &QTimer::timeout, host, [context] {
        auto& status = context->status;
        if (status.dwCurrentState == SERVICE_START_PENDING ||
            status.dwCurrentState == SERVICE_STOP_PENDING) {
            ++status.dwCheckPoint;
            SetServiceStatus(context->handle, &status);
        }
    });
    pending->start();
    QObject::connect(notify, &QWinEventNotifier::activated, host, [context, notify, host] {
        notify->setEnabled(false);
        auto& status = context->status;
        status.dwCurrentState = SERVICE_STOP_PENDING;
        status.dwControlsAccepted = 0;
        status.dwCheckPoint = 1;
        status.dwWaitHint = 30000;
        SetServiceStatus(context->handle, &status);
        host->shutdown();
    });
    QObject::connect(host, &ServiceHost::ready, host, [context] {
        auto& status = context->status;
        status.dwCurrentState = SERVICE_RUNNING;
        status.dwControlsAccepted =
            SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_PRESHUTDOWN;
        status.dwCheckPoint = 0;
        status.dwWaitHint = 0;
        SetServiceStatus(context->handle, &status);
    });
    QObject::connect(host, &ServiceHost::drained, host, [context] { finishService(context, {}); });
    QObject::connect(host, &ServiceHost::fatal, host,
                     [context](const QString& error) { finishService(context, error); });
    host->initialize();
} catch (const std::exception& error) {
    finishService(context, QString::fromUtf8(error.what()));
} catch (...) {
    finishService(context, QStringLiteral("Unexpected service initialization failure."));
}

void WINAPI serviceEntry(DWORD /*argc*/, wchar_t** /*argv*/) try {
    const auto context = std::make_shared<ServiceContext>();
    blank::windows::check(context->stop != nullptr && context->done != nullptr);
    context->handle = RegisterServiceCtrlHandlerExW(serviceName, serviceControl, context.get());
    blank::windows::check(context->handle != nullptr);
    context->status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    context->status.dwCurrentState = SERVICE_START_PENDING;
    context->status.dwWaitHint = 30000;
    blank::windows::check(SetServiceStatus(context->handle, &context->status) != FALSE);
    QMetaObject::invokeMethod(
        QCoreApplication::instance(), [context] { startRuntime(context); }, Qt::QueuedConnection);

    WaitForSingleObject(context->done.get(), INFINITE);
} catch (...) {
    QMetaObject::invokeMethod(
        QCoreApplication::instance(), [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
}
} // namespace

int main(int argc, char* argv[]) try {
    qputenv("QT_FATAL_WARNINGS", "1");
    qputenv("QT_COMMAND_LINE_PARSER_NO_GUI_MESSAGE_BOXES", "1");
    QCoreApplication const application(argc, argv);
    if (argc == 2) {
        const auto command = QCoreApplication::arguments().value(1);
        QString error;
        if (command == QStringLiteral("--install")) {
            error = installService();
        } else if (command == QStringLiteral("--uninstall")) {
            error = uninstallService();
        } else if (command == QStringLiteral("--cleanup-tun")) {
            if (!runningAsSystem() || QCoreApplication::applicationDirPath().compare(
                                          installedServiceDirectory(), Qt::CaseInsensitive) != 0) {
                return 2;
            }
            verifyTrustedPath(QCoreApplication::applicationFilePath());
            error = releaseTun(serviceDataDirectory() + QStringLiteral("/tun.json"));
            if (!error.isEmpty()) {
                QSaveFile record(serviceDataDirectory() + QStringLiteral("/cleanup-error.txt"));
                record.setDirectWriteFallback(false);
                const auto bytes = error.toUtf8();
                if (record.open(QIODevice::WriteOnly) && record.write(bytes) == bytes.size()) {
                    static_cast<void>(record.commit());
                }
                return 1;
            }
            return 0;
        } else {
            return 2;
        }
        if (!error.isEmpty()) {
            try {
                const auto directory = installedServiceDirectory();
                verifyTrustedPath(directory);
                QSaveFile log(directory + QStringLiteral("/setup-error.txt"));
                log.setDirectWriteFallback(false);
                const auto bytes = error.toUtf8();
                if (log.open(QIODevice::WriteOnly) && log.write(bytes) == bytes.size()) {
                    static_cast<void>(log.commit());
                }
            } catch (...) {
                OutputDebugStringW(
                    L"BlankQuick setup failed; protected error record unavailable.\n");
            }
            return 1;
        }
        return 0;
    }
    if (argc != 1) {
        return 2;
    }
    if (!runningAsSystem()) {
        return 2;
    }
    qInstallMessageHandler(serviceMessage);
    const std::jthread dispatcher([] {
        std::wstring name(serviceName);
        std::array<SERVICE_TABLE_ENTRYW, 2> table{
            {
                {.lpServiceName = name.data(), .lpServiceProc = serviceEntry},
                {.lpServiceName = nullptr, .lpServiceProc = nullptr},
            },
        };
        if (StartServiceCtrlDispatcherW(table.data()) == FALSE) {
            QMetaObject::invokeMethod(
                QCoreApplication::instance(), [] { QCoreApplication::exit(1); },
                Qt::QueuedConnection);
        }
    });
    try {
        return QCoreApplication::exec();
    } catch (...) {
        TerminateProcess(GetCurrentProcess(), ERROR_EXCEPTION_IN_SERVICE);
        return 1;
    }
} catch (...) {
    return 1;
}
