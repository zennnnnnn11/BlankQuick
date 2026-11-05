#ifndef BLANK_QT_CHILD_HPP
#define BLANK_QT_CHILD_HPP

#include <QThread>

#include <QObject>
#include <concepts>
#include <memory>
#include <stdexcept>
#include <utility>

namespace blank::qt {
// Exception values retain standard copy semantics; this final leaf is caught by const reference.
class ChildParentError final : public std::logic_error { // clazy:exclude=copyable-polymorphic
  public:
    using std::logic_error::logic_error;
};

template <typename Child, typename... Arguments>
    requires std::derived_from<Child, QObject> && std::constructible_from<Child, Arguments...>
[[nodiscard]] Child& makeChild(QObject& parent, Arguments&&... arguments) {
    // 构造期间由 unique_ptr 兜底，确认 Qt 父对象接管成功后才 release。
    // 返回引用仅供观察/调用，所有权已经属于 parent；线程不匹配时提前拒绝。
    if (parent.thread() != QThread::currentThread()) {
        throw ChildParentError{"Qt child must be created in its requested parent's thread"};
    }
    auto child = std::make_unique<Child>(std::forward<Arguments>(arguments)...);
    child->setParent(&parent);
    if (child->parent() != &parent) {
        throw ChildParentError{"Qt child could not be assigned to its requested parent"};
    }
    auto& observer = *child;
    static_cast<void>(child.release());
    return observer;
}
} // namespace blank::qt

#endif
