#ifndef BLANK_QUICK_USER_PROXY_HPP
#define BLANK_QUICK_USER_PROXY_HPP

#include <QString>
#include <QtTypes>
#include <functional>

struct UserProxySnapshot {
    quint32 flags = 0;
    QString server;
    QString bypass;
    QString pac;
    bool operator==(const UserProxySnapshot&) const = default;
};

[[nodiscard]] UserProxySnapshot readUserProxy();
void writeUserProxy(const UserProxySnapshot& snapshot);
void notifyUserProxy();
[[nodiscard]] QString processBirth(quint32 processId);

enum class ProxyRestoreResult : quint8 {
    NoRecord,
    Restored,
    Conflict,
};

// The caller holds the per-user journal lock for the complete ownership interval.
class UserProxyTransaction final {
  public:
    using Reader = std::function<UserProxySnapshot()>;
    using Writer = std::function<void(const UserProxySnapshot&)>;
    explicit UserProxyTransaction(QString path, Reader read = readUserProxy,
                                  Writer write = writeUserProxy);
    void apply(quint16 port);
    [[nodiscard]] bool matches(quint16 port) const;
    [[nodiscard]] ProxyRestoreResult restore();

  private:
    QString path_;
    Reader read_;
    Writer write_;
};

#endif
