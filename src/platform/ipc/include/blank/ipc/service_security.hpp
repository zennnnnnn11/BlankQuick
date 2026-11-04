#ifndef BLANK_QUICK_SERVICE_SECURITY_HPP
#define BLANK_QUICK_SERVICE_SECURITY_HPP

#include <Windows.h>

#include <QString>
#include <QStringList>
#include <blank/windows/handle.hpp>

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
