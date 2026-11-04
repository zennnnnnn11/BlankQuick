#include <AclAPI.h>
#include <QDir>
#include <QFileInfo>
#include <Sddl.h>
#include <ShlObj.h>
#include <WtsApi32.h>
#include <algorithm>
#include <array>
#include <blank/ipc/service_security.hpp>
#include <cstddef>
#include <span>
#include <stdexcept>
#include <system_error>
#include <vector>

namespace {
blank::windows::Handle processToken(HANDLE process) {
    HANDLE token = nullptr;
    blank::windows::check(OpenProcessToken(process, TOKEN_QUERY, &token) != FALSE);
    return blank::windows::Handle(token);
}

std::vector<std::max_align_t> tokenInformation(HANDLE token, TOKEN_INFORMATION_CLASS kind) {
    DWORD size = 0;
    blank::windows::check(GetTokenInformation(token, kind, nullptr, 0, &size) == FALSE &&
                          GetLastError() == ERROR_INSUFFICIENT_BUFFER && size > 0 && size <= 65536);
    std::vector<std::max_align_t> bytes((size + sizeof(std::max_align_t) - 1) /
                                        sizeof(std::max_align_t));
    blank::windows::check(GetTokenInformation(token, kind, bytes.data(), size, &size) != FALSE);
    return bytes;
}

QString sidText(PSID sid) {
    wchar_t* text = nullptr;
    blank::windows::check(ConvertSidToStringSidW(sid, &text) != FALSE);
    const blank::windows::LocalMemory allocation(text);
    return QString::fromWCharArray(text);
}

bool trustedSid(PSID sid) {
    return IsWellKnownSid(sid, WinLocalSystemSid) != FALSE ||
           IsWellKnownSid(sid, WinBuiltinAdministratorsSid) != FALSE ||
           sidText(sid) ==
               QStringLiteral("S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464");
}

QString knownDirectory(KNOWNFOLDERID id) {
    wchar_t* path = nullptr;
    const auto result = SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &path);
    if (FAILED(result)) {
        throw std::runtime_error("Cannot locate the Windows installation directory.");
    }
    const auto value = QDir::fromNativeSeparators(QString::fromWCharArray(path));
    CoTaskMemFree(path);
    return value;
}
} // namespace

blank::windows::LocalMemory securityDescriptor(const QString& sddl) {
    void* descriptor = nullptr;
    const auto value = sddl.toStdWString();
    blank::windows::check(ConvertStringSecurityDescriptorToSecurityDescriptorW(
                              value.c_str(), SDDL_REVISION_1, &descriptor, nullptr) != FALSE);
    return blank::windows::LocalMemory(descriptor);
}

QString tokenLogonSid(HANDLE token) {
    const auto bytes = tokenInformation(token, TokenGroups);
    // GetTokenInformation returns this Windows ABI header followed by GroupCount entries.
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
    const auto* groups = reinterpret_cast<const TOKEN_GROUPS*>(bytes.data());
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
    const auto entries = std::span(groups->Groups, groups->GroupCount);
    const auto group = std::ranges::find_if(entries, [](const auto& entry) {
        return (entry.Attributes & SE_GROUP_LOGON_ID) == SE_GROUP_LOGON_ID;
    });
    if (group != entries.end()) {
        return sidText(group->Sid);
    }
    throw std::runtime_error("The peer has no interactive logon SID.");
}

QStringList loggedOnSids() {
    WTS_SESSION_INFOW* entries = nullptr;
    DWORD count = 0;
    blank::windows::check(
        WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &entries, &count) != FALSE);
    const auto freeSessions = [](WTS_SESSION_INFOW* value) { WTSFreeMemory(value); };
    const std::unique_ptr<WTS_SESSION_INFOW, decltype(freeSessions)> memory(entries, freeSessions);
    QStringList result;
    for (const auto& session : std::span(entries, count)) {
        if (session.SessionId == 0 ||
            (session.State != WTSActive && session.State != WTSConnected &&
             session.State != WTSDisconnected)) {
            continue;
        }
        HANDLE raw = nullptr;
        if (WTSQueryUserToken(session.SessionId, &raw) == FALSE) {
            if (GetLastError() == ERROR_NO_TOKEN) {
                continue;
            }
            blank::windows::check(false);
        }
        const blank::windows::Handle token(raw);
        result.append(tokenLogonSid(token.get()));
    }
    result.removeDuplicates();
    return result;
}

PipeIdentity identifyPipeClient(HANDLE pipe, const QStringList& allowed) {
    ULONG process = 0;
    ULONG session = 0;
    blank::windows::check(GetNamedPipeClientProcessId(pipe, &process) != FALSE);
    blank::windows::check(GetNamedPipeClientSessionId(pipe, &session) != FALSE);
    if (process == 0 || session == 0) {
        throw std::runtime_error("A service client must belong to an interactive session.");
    }
    blank::windows::Handle lifetime(
        OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process));
    blank::windows::check(lifetime != nullptr);
    const auto processAccess = processToken(lifetime.get());
    const auto processSid = tokenLogonSid(processAccess.get());
    blank::windows::check(ImpersonateNamedPipeClient(pipe) != FALSE);
    QString peerSid;
    try {
        HANDLE raw = nullptr;
        blank::windows::check(OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &raw) !=
                              FALSE);
        const blank::windows::Handle token(raw);
        peerSid = tokenLogonSid(token.get());
    } catch (...) {
        if (RevertToSelf() == FALSE) {
            TerminateProcess(GetCurrentProcess(), ERROR_ACCESS_DENIED);
        }
        throw;
    }
    if (RevertToSelf() == FALSE) {
        TerminateProcess(GetCurrentProcess(), ERROR_ACCESS_DENIED);
        throw std::runtime_error("Cannot leave pipe impersonation.");
    }
    DWORD actualSession = 0;
    blank::windows::check(ProcessIdToSessionId(process, &actualSession) != FALSE);
    if (actualSession != session || processSid != peerSid || !allowed.contains(peerSid) ||
        WaitForSingleObject(lifetime.get(), 0) != WAIT_TIMEOUT) {
        throw std::runtime_error("Pipe process, session and logon identity do not match.");
    }
    return {
        .process = process,
        .session = session,
        .logonSid = peerSid,
        .lifetime = std::move(lifetime),
    };
}

void verifyServicePipe(HANDLE pipe) {
    PSID owner = nullptr;
    void* descriptor = nullptr;
    const auto status = GetSecurityInfo(pipe, SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION, &owner,
                                        nullptr, nullptr, nullptr, &descriptor);
    const blank::windows::LocalMemory memory(descriptor);
    if (status != ERROR_SUCCESS || IsWellKnownSid(owner, WinLocalSystemSid) == FALSE) {
        throw std::runtime_error("The service pipe is not owned by SYSTEM.");
    }
    ULONG process = 0;
    ULONG session = 0;
    blank::windows::check(GetNamedPipeServerProcessId(pipe, &process) != FALSE);
    blank::windows::check(GetNamedPipeServerSessionId(pipe, &session) != FALSE);
    const auto closeService = [](SC_HANDLE value) { CloseServiceHandle(value); };
    using ServiceHandle = std::unique_ptr<SC_HANDLE__, decltype(closeService)>;
    const ServiceHandle manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT), closeService);
    blank::windows::check(manager != nullptr);
    const ServiceHandle service(OpenServiceW(manager.get(), serviceName, SERVICE_QUERY_STATUS),
                                closeService);
    blank::windows::check(service != nullptr);
    SERVICE_STATUS_PROCESS state{};
    DWORD bytes = 0;
    // QueryServiceStatusEx requires a byte buffer for the fixed Windows status structure.
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
    blank::windows::check(QueryServiceStatusEx(service.get(), SC_STATUS_PROCESS_INFO,
                                               reinterpret_cast<BYTE*>(&state), sizeof(state),
                                               &bytes) != FALSE);
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
    if (session != 0 || process == 0 || state.dwProcessId != process ||
        state.dwCurrentState != SERVICE_RUNNING) {
        throw std::runtime_error("The pipe server does not match the running SCM service.");
    }
}

bool runningAsSystem() {
    const auto token = processToken(GetCurrentProcess());
    const auto bytes = tokenInformation(token.get(), TokenUser);
    // The returned buffer is aligned storage for TOKEN_USER.
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
    const auto* user = reinterpret_cast<const TOKEN_USER*>(bytes.data());
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
    return IsWellKnownSid(user->User.Sid, WinLocalSystemSid) != FALSE;
}

bool elevatedProcess() {
    const auto token = processToken(GetCurrentProcess());
    TOKEN_ELEVATION value{};
    DWORD size = 0;
    blank::windows::check(
        GetTokenInformation(token.get(), TokenElevation, &value, sizeof(value), &size) != FALSE);
    return value.TokenIsElevated != 0;
}

void verifyTrustedPath(const QString& path) {
    auto current = QFileInfo(path).absoluteFilePath();
    const auto programFiles = knownDirectory(FOLDERID_ProgramFiles);
    const auto programData = knownDirectory(FOLDERID_ProgramData);
    while (!current.isEmpty()) {
        const auto wide = QDir::toNativeSeparators(current).toStdWString();
        const auto attributes = GetFileAttributesW(wide.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            throw std::runtime_error(
                "Trusted installation path is missing or traverses a reparse point.");
        }
        PSID owner = nullptr;
        ACL* acl = nullptr;
        void* descriptor = nullptr;
        const auto result = GetNamedSecurityInfoW(
            wide.c_str(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
            &owner, nullptr, &acl, nullptr, &descriptor);
        const blank::windows::LocalMemory memory(descriptor);
        if (result != ERROR_SUCCESS || acl == nullptr || !trustedSid(owner)) {
            throw std::runtime_error("Trusted installation has an unsafe owner or DACL.");
        }
        const bool sharedRoot = current.compare(programFiles, Qt::CaseInsensitive) == 0 ||
                                current.compare(programData, Qt::CaseInsensitive) == 0;
        if (sharedRoot) {
            break;
        }
        for (DWORD index = 0; index < acl->AceCount; ++index) {
            void* raw = nullptr;
            blank::windows::check(GetAce(acl, index, &raw) != FALSE);
            const auto* header = static_cast<const ACE_HEADER*>(raw);
            if (header->AceType == ACCESS_ALLOWED_ACE_TYPE &&
                (header->AceFlags & INHERIT_ONLY_ACE) == 0) {
                auto* entry = static_cast<ACCESS_ALLOWED_ACE*>(raw);
                constexpr DWORD writes = FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA |
                                         FILE_WRITE_ATTRIBUTES | FILE_DELETE_CHILD | DELETE |
                                         WRITE_DAC | WRITE_OWNER | GENERIC_ALL | GENERIC_WRITE;
                if ((entry->Mask & writes) != 0 && !trustedSid(&entry->SidStart)) {
                    throw std::runtime_error("Trusted installation permits an untrusted writer.");
                }
            } else if (header->AceType != ACCESS_ALLOWED_ACE_TYPE &&
                       header->AceType != ACCESS_DENIED_ACE_TYPE) {
                throw std::runtime_error("Unsupported installation access control entry.");
            }
        }
        const auto parent = QFileInfo(current).dir().absolutePath();
        if (parent == current || QDir(parent).isRoot()) {
            throw std::runtime_error(
                "Service files must stay within Program Files or ProgramData.");
        }
        current = parent;
    }
}

void protectDirectory(const QString& directory, bool readable) {
    verifyTrustedPath(QFileInfo(directory).dir().absolutePath());
    const auto descriptor = securityDescriptor(
        readable ? QStringLiteral("O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FRFX;;;BU)")
                 : QStringLiteral("O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)"));
    SECURITY_ATTRIBUTES attributes{
        .nLength = sizeof(SECURITY_ATTRIBUTES),
        .lpSecurityDescriptor = descriptor.get(),
        .bInheritHandle = FALSE,
    };
    const auto wide = QDir::toNativeSeparators(directory).toStdWString();
    if (CreateDirectoryW(wide.c_str(), &attributes) == FALSE &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        blank::windows::check(false);
    }
    verifyTrustedPath(directory);
}

QString installedServiceDirectory() {
    return knownDirectory(FOLDERID_ProgramFiles) + QStringLiteral("/BlankQuickService");
}

QString serviceDataDirectory() {
    return knownDirectory(FOLDERID_ProgramData) + QStringLiteral("/BlankQuickService");
}
