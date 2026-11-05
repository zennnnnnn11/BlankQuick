#ifndef BLANK_QUICK_USER_PROXY_HPP
#define BLANK_QUICK_USER_PROXY_HPP

#include <QtTypes>

#include <QString>
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

// 一次“保存现场 -> 接管 -> 有条件恢复”的事务。Reader/Writer 便于独立验证
// 恢复规则；生产默认实现操作当前用户 WinINet 设置，不修改整个机器的代理策略。
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
