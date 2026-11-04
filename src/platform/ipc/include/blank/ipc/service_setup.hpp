#ifndef BLANK_QUICK_SERVICE_SETUP_HPP
#define BLANK_QUICK_SERVICE_SETUP_HPP

#include <QString>

struct ServiceInstallation {
    bool installed = false;
    bool running = false;
    QString error;
};

[[nodiscard]] ServiceInstallation inspectService(bool start);
[[nodiscard]] QString installService();
[[nodiscard]] QString uninstallService();

#endif
