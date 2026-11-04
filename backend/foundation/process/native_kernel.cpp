#include <winsock2.h>

#include <windows.h>

#include <iphlpapi.h>

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QUuid>
#include <blank/process/native_kernel.hpp>
#include <blank/windows/handle.hpp>
#include <cstddef>
#include <cstring>
#include <exception>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
void requireWindows(bool success)
{
    if (!success)
    {
        throw std::runtime_error("Windows error " + std::to_string(GetLastError()));
    }
}

QString quote(const QString& argument)
{
    QString result = QStringLiteral("\"");
    qsizetype slashes = 0;
    for (const auto character : argument)
    {
        if (character == u'\\')
        {
            ++slashes;
            continue;
        }
        result += QString(slashes * (character == u'"' ? 2 : 1), u'\\');
        if (character == u'"')
        {
            result += u'\\';
        }
        result += character;
        slashes = 0;
    }
    return result + QString(slashes * 2, u'\\') + u'"';
}

class Attributes final
{
  public:
    explicit Attributes(HANDLE job) : job_(job)
    {
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        requireWindows(size != 0 && GetLastError() == ERROR_INSUFFICIENT_BUFFER);
        storage_.resize((size + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t));
        requireWindows(InitializeProcThreadAttributeList(get(), 1, 0, &size) != FALSE);
        if (UpdateProcThreadAttribute(get(), 0, PROC_THREAD_ATTRIBUTE_JOB_LIST,
                                      static_cast<void*>(&job_), sizeof(job_), nullptr,
                                      nullptr) == FALSE)
        {
            DeleteProcThreadAttributeList(get());
            requireWindows(false);
        }
    }

    ~Attributes()
    {
        DeleteProcThreadAttributeList(get());
    }

    Attributes(const Attributes&) = delete;
    Attributes& operator=(const Attributes&) = delete;
    Attributes(Attributes&&) = delete;
    Attributes& operator=(Attributes&&) = delete;

    [[nodiscard]] LPPROC_THREAD_ATTRIBUTE_LIST get() noexcept
    {
        // Win32 requires its opaque attribute list to live in caller-owned aligned storage.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): opaque Win32 storage.
        return reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage_.data());
    }

  private:
    std::vector<std::max_align_t> storage_;
    HANDLE job_ = nullptr;
};

blank::windows::Handle createInJob(HANDLE job, const QString& executable,
                                   const QStringList& arguments, const QString& directory,
                                   bool console)
{
    if (!QFileInfo(executable).isAbsolute() || !QFileInfo(executable).isFile())
    {
        throw std::runtime_error("Kernel or signal helper executable is missing.");
    }
    QString command = quote(QDir::toNativeSeparators(executable));
    for (const auto& argument : arguments)
    {
        command += u' ' + quote(argument);
    }
    if (command.size() >= 32767 || command.contains(QChar::Null))
    {
        throw std::runtime_error("Invalid Windows process command line.");
    }
    auto mutableCommand = command.toStdWString();
    const auto image = QDir::toNativeSeparators(executable).toStdWString();
    const auto workingDirectory = QDir::toNativeSeparators(directory).toStdWString();
    auto environment = QProcessEnvironment::systemEnvironment();
    const auto keys = environment.keys();
    for (const auto& key : keys)
    {
        // mihomo reads CLASH_* overrides and hooks before applying its configuration file.
        if (key.startsWith(QStringLiteral("CLASH_"), Qt::CaseInsensitive) ||
            key.compare(QStringLiteral("SKIP_SAFE_PATH_CHECK"), Qt::CaseInsensitive) == 0 ||
            key.compare(QStringLiteral("SAFE_PATHS"), Qt::CaseInsensitive) == 0)
        {
            environment.remove(key);
        }
    }
    auto entries = environment.toStringList();
    entries.sort(Qt::CaseInsensitive);
    auto environmentBlock = (entries.join(QChar::Null) + QChar::Null + QChar::Null).toStdWString();
    Attributes attributes(job);
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESHOWWINDOW;
    startup.StartupInfo.wShowWindow = SW_HIDE;
    startup.lpAttributeList = attributes.get();
    PROCESS_INFORMATION information{};
    const DWORD flags = EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT |
                        (console ? CREATE_NEW_CONSOLE : CREATE_NO_WINDOW);
    requireWindows(CreateProcessW(image.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE,
                                  flags, environmentBlock.data(), workingDirectory.c_str(),
                                  &startup.StartupInfo, &information) != FALSE);
    blank::windows::Handle process(information.hProcess);
    const blank::windows::Handle thread(information.hThread);
    BOOL inJob = FALSE;
    requireWindows(IsProcessInJob(process.get(), job, &inJob) != FALSE && inJob != FALSE);
    return process;
}

bool jobEmpty(HANDLE job)
{
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
    requireWindows(QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &accounting,
                                             sizeof(accounting), nullptr) != FALSE);
    return accounting.ActiveProcesses == 0;
}
} // namespace

class NativeKernel::Impl
{
  public:
    explicit Impl(const QString& root)
        : directory((root.isEmpty() ? QDir::tempPath() : root) +
                    QStringLiteral("/blank-quick-XXXXXX"))
    {
    }

    QTemporaryDir directory;
    QString token = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QString initialConfiguration;
    quint16 controllerPort = 0;
    blank::windows::Handle process;
    // Declared last: the non-inheritable job closes before process handles and runtime files.
    blank::windows::Handle job;
};

NativeKernel::NativeKernel(const KernelOptions& options)
    : impl_(std::make_unique<Impl>(options.workRoot))
{
    if (!impl_->directory.isValid())
    {
        throw std::runtime_error("Cannot create the kernel runtime directory.");
    }
    QTcpServer reservation;
    if (!reservation.listen(QHostAddress::LocalHost, options.port))
    {
        throw std::runtime_error("The controller port is already occupied.");
    }
    impl_->controllerPort = reservation.serverPort();
    QTcpServer mixedReservation;
    if (!mixedReservation.listen(QHostAddress::LocalHost, 0))
    {
        throw std::runtime_error("Cannot reserve a loopback mixed port.");
    }
    const QJsonObject configuration{
        {QStringLiteral("external-controller"), QStringLiteral("127.0.0.1:%1").arg(port())},
        {QStringLiteral("secret"), impl_->token},
        {QStringLiteral("mixed-port"), mixedReservation.serverPort()},
        {QStringLiteral("bind-address"), QStringLiteral("127.0.0.1")},
        {QStringLiteral("mode"), QStringLiteral("direct")},
        {QStringLiteral("find-process-mode"), QStringLiteral("always")},
        {QStringLiteral("log-level"), QStringLiteral("error")},
        {QStringLiteral("ipv6"), false},
        {QStringLiteral("allow-lan"), false},
        {QStringLiteral("dns"), QJsonObject{{QStringLiteral("enable"), false}}},
        {QStringLiteral("tun"), QJsonObject{{QStringLiteral("enable"), false}}},
    };
    const auto data = QJsonDocument(configuration).toJson(QJsonDocument::Compact);
    impl_->initialConfiguration = QString::fromUtf8(data);
    const auto configPath = impl_->directory.filePath(QStringLiteral("config.json"));
    QFile config(configPath);
    if (!config.open(QIODevice::WriteOnly) || config.write(data) != data.size())
    {
        throw std::runtime_error("Cannot write the kernel configuration.");
    }
    config.close();
    impl_->job.reset(CreateJobObjectW(nullptr, nullptr));
    requireWindows(impl_->job != nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    requireWindows(SetInformationJobObject(impl_->job.get(), JobObjectExtendedLimitInformation,
                                           &limits, sizeof(limits)) != FALSE);
    reservation.close();
    mixedReservation.close();
    impl_->process = createInJob(
        impl_->job.get(), options.executable,
        {QStringLiteral("-d"), impl_->directory.path(), QStringLiteral("-f"), configPath},
        impl_->directory.path(), true);
}

NativeKernel::~NativeKernel()
{
    if (impl_->process && WaitForSingleObject(impl_->process.get(), 0) == WAIT_TIMEOUT)
    {
        // Construction/active teardown happens on a worker. Controllers keep an
        // asynchronous cleanup owner alive; a normally stopped destructor never waits.
        static_cast<void>(stop({}));
    }
}

QString NativeKernel::workingDirectory() const
{
    return impl_->directory.path();
}

QString NativeKernel::providerCacheError() const
{
    const auto root = impl_->directory.filePath(QStringLiteral("providers"));
    const auto safe = [](const QString& path)
    {
        const auto wide = QDir::toNativeSeparators(path).toStdWString();
        const auto attributes = GetFileAttributesW(wide.c_str());
        return attributes == INVALID_FILE_ATTRIBUTES ||
               (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
    };
    if (!safe(root))
    {
        return QStringLiteral("Provider cache root is a reparse point.");
    }
    QDirIterator entries(root,
                         QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                         QDirIterator::Subdirectories);
    int count = 0;
    while (entries.hasNext())
    {
        if (++count > 4096 || !safe(entries.next()))
        {
            return QStringLiteral("Provider cache contains a reparse point or too many entries.");
        }
    }
    return {};
}

void* NativeKernel::processHandle() const noexcept
{
    return impl_->process.get();
}

quint32 NativeKernel::processId() const noexcept
{
    return GetProcessId(impl_->process.get());
}

quint16 NativeKernel::port() const noexcept
{
    return impl_->controllerPort;
}

const QString& NativeKernel::secret() const noexcept
{
    return impl_->token;
}

const QString& NativeKernel::initialConfiguration() const noexcept
{
    return impl_->initialConfiguration;
}

QString NativeKernel::stop(const QString& signalHelper)
{
    try
    {
        if (WaitForSingleObject(impl_->process.get(), 0) == WAIT_TIMEOUT)
        {
            FILETIME creation{};
            FILETIME exit{};
            FILETIME kernel{};
            FILETIME user{};
            requireWindows(
                GetProcessTimes(impl_->process.get(), &creation, &exit, &kernel, &user) != FALSE);
            const quint64 created =
                (quint64{creation.dwHighDateTime} << 32U) | creation.dwLowDateTime;
            try
            {
                const auto helper =
                    createInJob(impl_->job.get(), signalHelper,
                                {QString::number(processId()), QString::number(created)},
                                impl_->directory.path(), false);
                WaitForSingleObject(helper.get(), 500);
                WaitForSingleObject(impl_->process.get(), 1500);
            }
            catch (const std::exception&)
            {
                // Console signaling failed: immediately use the mandatory owned-job fallback.
                requireWindows(TerminateJobObject(impl_->job.get(), 1) != FALSE);
            }
        }
        if (!jobEmpty(impl_->job.get()))
        {
            requireWindows(TerminateJobObject(impl_->job.get(), 1) != FALSE);
        }
        requireWindows(WaitForSingleObject(impl_->process.get(), 2000) == WAIT_OBJECT_0);
        // Termination is asynchronous; confirm the entire job, not just the root process.
        for (int attempt = 0; attempt < 200; ++attempt)
        {
            if (jobEmpty(impl_->job.get()))
            {
                if (impl_->directory.remove())
                {
                    impl_->directory.setAutoRemove(false);
                }
                return {};
            }
            Sleep(10); // Worker thread only; never blocks the GUI thread.
        }
        return QStringLiteral("Timed out waiting for the owned kernel job to exit.");
    }
    catch (const std::exception& error)
    {
        return QString::fromUtf8(error.what());
    }
}

KernelLaunch launchKernel(const KernelOptions& options)
{
    try
    {
        KernelLaunch result;
        result.kernel = std::make_shared<NativeKernel>(options);
        return result;
    }
    catch (const std::exception& error)
    {
        return {.kernel = {}, .error = QString::fromUtf8(error.what())};
    }
}

ListenerCheck checkKernelListener(quint16 port, quint32 processId)
{
    DWORD size = sizeof(MIB_TCPTABLE);
    auto status =
        GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0);
    std::vector<std::byte> storage;
    for (int attempt = 0; attempt < 3 && status == ERROR_INSUFFICIENT_BUFFER; ++attempt)
    {
        storage.resize(size);
        status = GetExtendedTcpTable(storage.data(), &size, FALSE, AF_INET,
                                     TCP_TABLE_OWNER_PID_LISTENER, 0);
    }
    if (status != NO_ERROR || storage.size() < sizeof(DWORD))
    {
        return {.owned = false, .error = QStringLiteral("TCP owner query failed: %1").arg(status)};
    }
    DWORD count = 0;
    std::memcpy(&count, storage.data(), sizeof(count));
    const auto offset = offsetof(MIB_TCPTABLE_OWNER_PID, table);
    if (count > (storage.size() - offset) / sizeof(MIB_TCPROW_OWNER_PID))
    {
        return {.owned = false, .error = QStringLiteral("Invalid TCP owner table size.")};
    }
    for (DWORD index = 0; index < count; ++index)
    {
        MIB_TCPROW_OWNER_PID row{};
        std::memcpy(&row,
                    std::span(storage).subspan(offset + (index * sizeof(row)), sizeof(row)).data(),
                    sizeof(row));
        if (row.dwOwningPid == processId && row.dwLocalAddr == htonl(INADDR_LOOPBACK) &&
            ntohs(static_cast<u_short>(row.dwLocalPort)) == port)
        {
            return {.owned = true, .error = {}};
        }
    }
    return {};
}

QString runOwnedTool(const QString& executable, const QStringList& arguments,
                     const QString& directory, int timeoutMs)
try
{
    if (timeoutMs <= 0 || timeoutMs > 30000)
    {
        return QStringLiteral("Invalid maintenance deadline.");
    }
    const blank::windows::Handle job(CreateJobObjectW(nullptr, nullptr));
    requireWindows(job != nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    requireWindows(SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits,
                                           sizeof(limits)) != FALSE);
    const auto process = createInJob(job.get(), executable, arguments, directory, false);
    const auto wait = WaitForSingleObject(process.get(), static_cast<DWORD>(timeoutMs));
    if (wait != WAIT_OBJECT_0)
    {
        requireWindows(TerminateJobObject(job.get(), ERROR_TIMEOUT) != FALSE);
        WaitForSingleObject(process.get(), 2000);
        return QStringLiteral(
            "The isolated TUN cleanup worker exceeded its deadline; recovery was retained.");
    }
    DWORD exit = 0;
    requireWindows(GetExitCodeProcess(process.get(), &exit) != FALSE);
    const auto end = GetTickCount64() + 2000;
    while (!jobEmpty(job.get()) && GetTickCount64() < end)
    {
        Sleep(10);
    }
    if (!jobEmpty(job.get()))
    {
        requireWindows(TerminateJobObject(job.get(), ERROR_PROCESS_ABORTED) != FALSE);
        return QStringLiteral("Maintenance descendants have not exited.");
    }
    if (exit != 0)
    {
        QFile file(directory + QStringLiteral("/cleanup-error.txt"));
        if (file.open(QIODevice::ReadOnly))
        {
            const auto error = QString::fromUtf8(file.read(4096));
            if (!error.isEmpty())
            {
                return error;
            }
        }
        return QStringLiteral("TUN cleanup worker failed.");
    }
    return {};
}
catch (const std::exception& error)
{
    return QString::fromUtf8(error.what());
}
