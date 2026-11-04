#include <Windows.h>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <array>
#include <blank/process/native_kernel.hpp>
#include <blank/user_proxy/user_proxy.hpp>
#include <blank/windows/handle.hpp>
#include <memory>
#include <stdexcept>

namespace
{
void report(const QString& state, const QString& message)
{
    const auto bytes = QJsonDocument(QJsonObject{
                                         {QStringLiteral("state"), state},
                                         {QStringLiteral("message"), message},
                                     })
                           .toJson(QJsonDocument::Compact) +
                       '\n';
    DWORD written = 0;
    // A dead owner may already have closed stdout. Restoration must still run.
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), bytes.constData(), static_cast<DWORD>(bytes.size()),
              &written, nullptr);
}

blank::windows::Handle watchProcess(const QString& id, const QString& birth)
{
    bool valid = false;
    const auto pid = id.toUInt(&valid);
    if (!valid || pid == 0)
    {
        throw std::runtime_error("Invalid watched process identity.");
    }
    blank::windows::Handle handle(
        OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    if (!handle || GetProcessTimes(handle.get(), &creation, &exit, &kernel, &user) == FALSE ||
        QString::number((quint64{creation.dwHighDateTime} << 32U) | creation.dwLowDateTime) !=
            birth ||
        WaitForSingleObject(handle.get(), 0) != WAIT_TIMEOUT)
    {
        throw std::runtime_error("The watched process is no longer the expected live process.");
    }
    return handle;
}

bool pipeClosed(UserProxyTransaction const& transaction, quint16 port)
{
    DWORD available = 0;
    auto* const input = GetStdHandle(STD_INPUT_HANDLE);
    if (PeekNamedPipe(input, nullptr, 0, nullptr, &available, nullptr) == FALSE)
    {
        return true;
    }
    if (available > 0)
    {
        char command = 0;
        DWORD read = 0;
        if (ReadFile(input, &command, 1, &read, nullptr) == FALSE || read != 1 || command != 'v')
        {
            return true;
        }
        try
        {
            if (transaction.matches(port))
            {
                notifyUserProxy();
                report(QStringLiteral("checked"), QStringLiteral("系统代理已核验并刷新通知"));
            }
            else
            {
                report(QStringLiteral("foreign"),
                       QStringLiteral("系统代理已被外部修改，未覆盖其他配置"));
            }
        }
        catch (const std::exception& error)
        {
            report(QStringLiteral("check-error"), QString::fromLocal8Bit(error.what()));
        }
    }
    return false;
}

int restore(UserProxyTransaction& transaction)
{
    if (transaction.restore() == ProxyRestoreResult::Conflict)
    {
        report(QStringLiteral("conflict"),
               QStringLiteral("系统代理已被外部修改，未覆盖；恢复记录已保留"));
        return 3;
    }
    report(QStringLiteral("restored"), QStringLiteral("系统代理已还原"));
    return 0;
}
} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication const application(argc, argv);
    const auto arguments = QCoreApplication::arguments();
    if (arguments.size() != 3 && arguments.size() != 8)
    {
        return 2;
    }
    const auto& path = arguments.at(2);
    try
    {
        DWORD session = 0;
        if (ProcessIdToSessionId(GetCurrentProcessId(), &session) == FALSE || session == 0)
        {
            throw std::runtime_error("WinINet requires an interactive user session.");
        }
        if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        {
            throw std::runtime_error("Cannot create the proxy recovery directory.");
        }
        QLockFile lock(path + QStringLiteral(".lock"));
        if (!lock.tryLock(0))
        {
            report(QStringLiteral("blocked"), QStringLiteral("另一实例正在管理系统代理"));
            return 4;
        }
        UserProxyTransaction transaction(path);
        if (arguments.at(1) == QStringLiteral("recover") && arguments.size() == 3)
        {
            return restore(transaction);
        }
        if (arguments.at(1) != QStringLiteral("guard") || arguments.size() != 8)
        {
            return 2;
        }
        const auto owner = watchProcess(arguments.at(3), arguments.at(4));
        const auto core = watchProcess(arguments.at(5), arguments.at(6));
        bool valid = false;
        const auto port = arguments.at(7).toUShort(&valid);
        const auto listener = checkKernelListener(port, arguments.at(5).toUInt());
        if (!valid || port == 0 || !listener.owned || !listener.error.isEmpty())
        {
            throw std::runtime_error("The mixed port does not belong to the current kernel.");
        }
        if (transaction.restore() == ProxyRestoreResult::Conflict)
        {
            report(QStringLiteral("conflict"),
                   QStringLiteral("待恢复快照与现场不一致，未接管代理"));
            return 3;
        }
        try
        {
            transaction.apply(port);
            report(QStringLiteral("owned"), QStringLiteral("系统代理已开启"));
            const std::array handles{owner.get(), core.get()};
            while (!pipeClosed(transaction, port))
            {
                const auto wait = WaitForMultipleObjects(static_cast<DWORD>(handles.size()),
                                                         handles.data(), FALSE, 100);
                if (wait != WAIT_TIMEOUT)
                {
                    break;
                }
            }
        }
        catch (const std::exception& error)
        {
            report(QStringLiteral("error"), QString::fromLocal8Bit(error.what()));
            // A failed set/notification/readback can occur after the actual mutation.
            static_cast<void>(restore(transaction));
            return 5;
        }
        return restore(transaction);
    }
    catch (const std::exception& error)
    {
        report(QStringLiteral("error"), QString::fromLocal8Bit(error.what()));
        return 5;
    }
}
