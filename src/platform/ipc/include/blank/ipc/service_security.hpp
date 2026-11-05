#ifndef BLANK_QUICK_SERVICE_SECURITY_HPP
#define BLANK_QUICK_SERVICE_SECURITY_HPP

#include <Windows.h>

#include <QString>
#include <QStringList>
#include <blank/windows/handle.hpp>

// 由系统查询与管道模拟令牌共同确认的对端身份，不取自客户端 JSON。
// lifetime 拥有进程观察句柄，用来检测原客户端退出，避免只靠可复用的 PID 判断存活。
struct PipeIdentity {
    quint32 process = 0;
    quint32 session = 0;
    QString logonSid;
    blank::windows::Handle lifetime;
};

inline constexpr auto serviceName = L"BlankQuickService";
inline constexpr auto servicePipe = L"\\\\.\\pipe\\BlankQuick.Control.v1";

[[nodiscard]] blank::windows::LocalMemory securityDescriptor(const QString& sddl);
[[nodiscard]] QString tokenLogonSid(HANDLE token);
[[nodiscard]] QStringList loggedOnSids();
[[nodiscard]] PipeIdentity identifyPipeClient(HANDLE pipe, const QStringList& allowed);
void verifyServicePipe(HANDLE pipe);
[[nodiscard]] bool runningAsSystem();
[[nodiscard]] bool elevatedProcess();
void protectDirectory(const QString& directory, bool readable);
void verifyTrustedPath(const QString& path);
[[nodiscard]] QString installedServiceDirectory();
[[nodiscard]] QString serviceDataDirectory();

#endif
