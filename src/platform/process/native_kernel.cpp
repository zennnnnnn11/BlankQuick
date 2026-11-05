#include <winsock2.h>
#include <ws2tcpip.h>

#include <windows.h>

#include <iphlpapi.h>

#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkProxy>
#include <QProcessEnvironment>
#include <QUuid>
#include <algorithm>
#include <array>
#include <blank/process/native_kernel.hpp>
#include <blank/windows/handle.hpp>
#include <concepts>
#include <cstddef>
#include <cstring>
#include <exception>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "kernel_workspace.hpp"

namespace {
template <typename Row, typename Predicate>
    requires(std::same_as<Row, MIB_TCPROW_OWNER_PID> || std::same_as<Row, MIB_TCP6ROW_OWNER_PID>) &&
            std::predicate<Predicate, const Row&>
ListenerCheck queryTcpRows(ULONG family, TCP_TABLE_CLASS kind, const Predicate& matches) {
    DWORD size = 0;
    auto status = GetExtendedTcpTable(nullptr, &size, FALSE, family, kind, 0);
    std::vector<std::byte> storage;
    for (int attempt = 0; attempt < 3 && status == ERROR_INSUFFICIENT_BUFFER; ++attempt) {
        storage.resize(size);
        status = GetExtendedTcpTable(storage.data(), &size, FALSE, family, kind, 0);
    }
    using Table = std::conditional_t<std::same_as<Row, MIB_TCPROW_OWNER_PID>,
                                     MIB_TCPTABLE_OWNER_PID, MIB_TCP6TABLE_OWNER_PID>;
    constexpr auto offset = offsetof(Table, table);
    if (status != NO_ERROR || storage.size() < offset) {
        return {.owned = false, .error = QStringLiteral("TCP owner query failed: %1").arg(status)};
    }
    DWORD count = 0;
    std::memcpy(&count, storage.data(), sizeof(count));
    if (count > (storage.size() - offset) / sizeof(Row)) {
        return {.owned = false, .error = QStringLiteral("Invalid TCP owner table size.")};
    }
    for (DWORD index = 0; index < count; ++index) {
        Row row{};
        std::memcpy(&row,
                    std::span(storage).subspan(offset + (index * sizeof(row)), sizeof(row)).data(),
                    sizeof(row));
        if (matches(row)) {
            return {.owned = true, .error = {}};
        }
    }
    return {};
}

constexpr bool mappedLoopback(std::span<const UCHAR, 16> address) noexcept {
    constexpr auto expected =
        std::to_array<UCHAR>({0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 127, 0, 0, 1});
    return std::ranges::equal(address, expected);
}

void requireWindows(bool success) {
    if (!success) {
        throw std::runtime_error("Windows error " + std::to_string(GetLastError()));
    }
}

QString quote(const QString& argument) {
    // Windows 命令行不是 argv 数组：引号前和结尾的反斜杠必须成倍转义。
    // 所有参数走同一规则，避免路径中的空格/引号改变子进程看到的参数边界。
    QString result = QStringLiteral("\"");
    qsizetype slashes = 0;
    for (const auto character : argument) {
        if (character == u'\\') {
            ++slashes;
            continue;
        }
        result += QString(slashes * (character == u'"' ? 2 : 1), u'\\');
        if (character == u'"') {
            result += u'\\';
        }
        result += character;
        slashes = 0;
    }
    return result + QString(slashes * 2, u'\\') + u'"';
}

class Attributes final {
  public:
    explicit Attributes(HANDLE job) : job_(job) {
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        requireWindows(size != 0 && GetLastError() == ERROR_INSUFFICIENT_BUFFER);
        storage_.resize((size + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t));
        requireWindows(InitializeProcThreadAttributeList(get(), 1, 0, &size) != FALSE);
        if (UpdateProcThreadAttribute(get(), 0, PROC_THREAD_ATTRIBUTE_JOB_LIST,
                                      static_cast<void*>(&job_), sizeof(job_), nullptr,
                                      nullptr) == FALSE) {
            DeleteProcThreadAttributeList(get());
            requireWindows(false);
        }
    }

    ~Attributes() {
        DeleteProcThreadAttributeList(get());
    }

    Attributes(const Attributes&) = delete;
    Attributes& operator=(const Attributes&) = delete;
    Attributes(Attributes&&) = delete;
    Attributes& operator=(Attributes&&) = delete;

    [[nodiscard]] LPPROC_THREAD_ATTRIBUTE_LIST get() noexcept {

        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): opaque Win32 storage.
        return reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage_.data());
    }

  private:
    std::vector<std::max_align_t> storage_;
    HANDLE job_ = nullptr;
};

blank::windows::Handle createInJob(HANDLE job, const QString& executable,
                                   const QStringList& arguments, const QString& directory,
                                   bool console) {
    if (!QFileInfo(executable).isAbsolute() || !QFileInfo(executable).isFile()) {
        throw std::runtime_error("Kernel or signal helper executable is missing.");
    }
    QString command = quote(QDir::toNativeSeparators(executable));
    for (const auto& argument : arguments) {
        command += u' ' + quote(argument);
    }
    if (command.size() >= 32767 || command.contains(QChar::Null)) {
        throw std::runtime_error("Invalid Windows process command line.");
    }
    auto mutableCommand = command.toStdWString();
    const auto image = QDir::toNativeSeparators(executable).toStdWString();
    const auto workingDirectory = QDir::toNativeSeparators(directory).toStdWString();
    auto environment = QProcessEnvironment::systemEnvironment();
    const auto keys = environment.keys();
    for (const auto& key : keys) {

        if (key.startsWith(QStringLiteral("CLASH_"), Qt::CaseInsensitive) ||
            key.compare(QStringLiteral("SKIP_SAFE_PATH_CHECK"), Qt::CaseInsensitive) == 0 ||
            key.compare(QStringLiteral("SAFE_PATHS"), Qt::CaseInsensitive) == 0) {
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

bool jobEmpty(HANDLE job) {
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
    requireWindows(QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &accounting,
                                             sizeof(accounting), nullptr) != FALSE);
    return accounting.ActiveProcesses == 0;
}

blank::windows::Handle createKernelJob() {
    blank::windows::Handle job(CreateJobObjectW(nullptr, nullptr));
    requireWindows(job != nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    requireWindows(SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits,
                                           sizeof(limits)) != FALSE);
    return job;
}
} // namespace

class NativeKernel::Impl {
  public:
    explicit Impl(const QString& root) : workspace(root, createKernelJob()) {}

    blank::process_detail::KernelWorkspace workspace;
    QString token = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QString initialConfiguration;
    quint16 controllerPort = 0;
    blank::windows::Handle process;
};

NativeKernel::NativeKernel(const KernelOptions& options)
    : impl_(std::make_unique<Impl>(options.workRoot)) {
    QTcpServer reservation;
    if (!reservation.listen(QHostAddress::LocalHost, options.port)) {
        throw std::runtime_error("The controller port is already occupied.");
    }
    impl_->controllerPort = reservation.serverPort();
    QTcpServer mixedReservation;
    if (!mixedReservation.listen(QHostAddress::LocalHost, 0)) {
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
        {
            QStringLiteral("dns"),
            QJsonObject{
                {QStringLiteral("enable"), true},
                {QStringLiteral("enhanced-mode"), QStringLiteral("redir-host")},
                {QStringLiteral("default-nameserver"), QJsonArray{QStringLiteral("system")}},
                {QStringLiteral("nameserver"), QJsonArray{QStringLiteral("system")}},
            },
        },
        {QStringLiteral("tun"), QJsonObject{{QStringLiteral("enable"), false}}},
    };
    const auto data = QJsonDocument(configuration).toJson(QJsonDocument::Compact);
    impl_->initialConfiguration = QString::fromUtf8(data);
    const auto configPath = impl_->workspace.directory().filePath(QStringLiteral("config.json"));
    QFile config(configPath);
    if (!config.open(QIODevice::WriteOnly) || config.write(data) != data.size()) {
        throw std::runtime_error("Cannot write the kernel configuration.");
    }
    config.close();
    reservation.close();
    mixedReservation.close();
    impl_->process = createInJob(impl_->workspace.job(), options.executable,
                                 {
                                     QStringLiteral("-d"),
                                     impl_->workspace.directory().path(),
                                     QStringLiteral("-f"),
                                     configPath,
                                 },
                                 impl_->workspace.directory().path(), true);
}

NativeKernel::~NativeKernel() {
    // 保留主进程的退出等待；主进程已退出时，workspace 析构仍会检查整个 Job，
    // 回收残留后代。构造未完成和 stop 失败也由同一个资源守卫兜底。
    try {
        if (WaitForSingleObject(impl_->process.get(), 0) == WAIT_TIMEOUT) {
            static_cast<void>(stop({}));
        }
    } catch (const std::exception&) {
        // 连错误字符串都无法分配时，仍由 workspace 析构执行无异常的资源兜底。
        impl_.reset();
    }
}

QString NativeKernel::workingDirectory() const {
    return impl_->workspace.directory().path();
}

QString NativeKernel::providerCacheError() const {
    const auto root = impl_->workspace.directory().filePath(QStringLiteral("providers"));
    const auto safe = [](const QString& path) {
        const auto wide = QDir::toNativeSeparators(path).toStdWString();
        const auto attributes = GetFileAttributesW(wide.c_str());
        return attributes == INVALID_FILE_ATTRIBUTES ||
               (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
    };
    if (!safe(root)) {
        return QStringLiteral("Provider cache root is a reparse point.");
    }
    QDirIterator entries(root,
                         QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                         QDirIterator::Subdirectories);
    int count = 0;
    while (entries.hasNext()) {
        if (++count > 4096 || !safe(entries.next())) {
            return QStringLiteral("Provider cache contains a reparse point or too many entries.");
        }
    }
    return {};
}

void* NativeKernel::processHandle() const noexcept {
    return impl_->process.get();
}

quint32 NativeKernel::processId() const noexcept {
    return GetProcessId(impl_->process.get());
}

quint16 NativeKernel::port() const noexcept {
    return impl_->controllerPort;
}

const QString& NativeKernel::secret() const noexcept {
    return impl_->token;
}

const QString& NativeKernel::initialConfiguration() const noexcept {
    return impl_->initialConfiguration;
}

QString NativeKernel::stop(const QString& signalHelper) {
    // 优先请求正常退出，超时或辅助程序失败后终止本对象拥有的 Job。
    // 辅助程序同时收到 PID 和创建时间，防止 PID 被复用后向无关进程发送信号。
    // 只有确认整个 Job 已空才清理临时目录；单个主进程退出不代表子进程都结束。
    try {
        if (WaitForSingleObject(impl_->process.get(), 0) == WAIT_TIMEOUT) {
            FILETIME creation{};
            FILETIME exit{};
            FILETIME kernel{};
            FILETIME user{};
            requireWindows(
                GetProcessTimes(impl_->process.get(), &creation, &exit, &kernel, &user) != FALSE);
            const quint64 created =
                (quint64{creation.dwHighDateTime} << 32U) | creation.dwLowDateTime;
            try {
                const auto helper =
                    createInJob(impl_->workspace.job(), signalHelper,
                                {QString::number(processId()), QString::number(created)},
                                impl_->workspace.directory().path(), false);
                WaitForSingleObject(helper.get(), 500);
                WaitForSingleObject(impl_->process.get(), 1500);
            } catch (const std::exception&) {

                requireWindows(TerminateJobObject(impl_->workspace.job(), 1) != FALSE);
            }
        }
        if (!jobEmpty(impl_->workspace.job())) {
            requireWindows(TerminateJobObject(impl_->workspace.job(), 1) != FALSE);
        }
        requireWindows(WaitForSingleObject(impl_->process.get(), 2000) == WAIT_OBJECT_0);

        impl_->workspace.removeAfterJobExit();
        return {};
    } catch (const std::exception& error) {
        return QString::fromUtf8(error.what());
    }
}

KernelLaunch launchKernel(const KernelOptions& options) {
    try {
        KernelLaunch result;
        result.kernel = std::make_shared<NativeKernel>(options);
        return result;
    } catch (const std::exception& error) {
        return {.kernel = {}, .error = QString::fromUtf8(error.what())};
    }
}

ListenerCheck checkKernelListener(quint16 port, quint32 processId) {
    return queryTcpRows<MIB_TCPROW_OWNER_PID>(
        AF_INET, TCP_TABLE_OWNER_PID_LISTENER, [&](const auto& row) noexcept {
            return row.dwOwningPid == processId && row.dwLocalAddr == htonl(INADDR_LOOPBACK) &&
                   ntohs(static_cast<u_short>(row.dwLocalPort)) == port;
        });
}

ListenerCheck checkProxyListener(quint16 port, quint32 processId) {
    // “端口能连通”不能证明属于本内核。建立一条无代理的本地连接，再按其
    // 两端端口和目标 PID 查系统 TCP 表，兼容 IPv4 映射到 IPv6 的监听情况。
    if (port == 0 || processId == 0) {
        return {};
    }

    QTcpSocket connection;
    connection.setProxy(QNetworkProxy::NoProxy);
    connection.connectToHost(QHostAddress::LocalHost, port);
    if (!connection.waitForConnected(1500)) {
        return {.owned = false, .error = connection.errorString()};
    }
    const auto peerPort = connection.localPort();
    auto ipv4 = queryTcpRows<MIB_TCPROW_OWNER_PID>(
        AF_INET, TCP_TABLE_OWNER_PID_CONNECTIONS, [&](const auto& row) noexcept {
            return row.dwState == MIB_TCP_STATE_ESTAB && row.dwOwningPid == processId &&
                   row.dwLocalAddr == htonl(INADDR_LOOPBACK) &&
                   row.dwRemoteAddr == htonl(INADDR_LOOPBACK) &&
                   ntohs(static_cast<u_short>(row.dwLocalPort)) == port &&
                   ntohs(static_cast<u_short>(row.dwRemotePort)) == peerPort;
        });
    if (ipv4.owned || !ipv4.error.isEmpty()) {
        return ipv4;
    }
    return queryTcpRows<MIB_TCP6ROW_OWNER_PID>(
        AF_INET6, TCP_TABLE_OWNER_PID_CONNECTIONS, [&](const auto& row) noexcept {
            return row.dwState == MIB_TCP_STATE_ESTAB && row.dwOwningPid == processId &&
                   mappedLoopback(row.ucLocalAddr) && mappedLoopback(row.ucRemoteAddr) &&
                   ntohs(static_cast<u_short>(row.dwLocalPort)) == port &&
                   ntohs(static_cast<u_short>(row.dwRemotePort)) == peerPort;
        });
}

QString runOwnedTool(const QString& executable, const QStringList& arguments,
                     const QString& directory, int timeoutMs) try {
    // 维护工具也放进独立 Job 并设硬期限。即使工具创建了子进程，超时回收
    // 仍限制在本次拥有的进程树内，不按进程名全局终止其他实例。
    if (timeoutMs <= 0 || timeoutMs > 30000) {
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
    if (wait != WAIT_OBJECT_0) {
        requireWindows(TerminateJobObject(job.get(), ERROR_TIMEOUT) != FALSE);
        WaitForSingleObject(process.get(), 2000);
        return QStringLiteral(
            "The isolated TUN cleanup worker exceeded its deadline; recovery was retained.");
    }
    DWORD exit = 0;
    requireWindows(GetExitCodeProcess(process.get(), &exit) != FALSE);
    const auto end = GetTickCount64() + 2000;
    while (!jobEmpty(job.get()) && GetTickCount64() < end) {
        Sleep(10);
    }
    if (!jobEmpty(job.get())) {
        requireWindows(TerminateJobObject(job.get(), ERROR_PROCESS_ABORTED) != FALSE);
        return QStringLiteral("Maintenance descendants have not exited.");
    }
    if (exit != 0) {
        QFile file(directory + QStringLiteral("/cleanup-error.txt"));
        if (file.open(QIODevice::ReadOnly)) {
            const auto error = QString::fromUtf8(file.read(4096));
            if (!error.isEmpty()) {
                return error;
            }
        }
        return QStringLiteral("TUN cleanup worker failed.");
    }
    return {};
} catch (const std::exception& error) {
    return QString::fromUtf8(error.what());
}
