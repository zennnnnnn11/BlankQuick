#ifndef BLANK_QT_CHILD_HPP
#define BLANK_QT_CHILD_HPP

#include <QObject>
#include <QThread>
#include <concepts>
#include <memory>
#include <stdexcept>
#include <utility>

namespace blank::qt {
class ChildParentError final : public std::logic_error {
  public:
    using std::logic_error::logic_error;
};

template <typename Child, typename... Arguments>
    requires std::derived_from<Child, QObject> && std::constructible_from<Child, Arguments...>
[[nodiscard]] Child& makeChild(QObject& parent, Arguments&&... arguments) {
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
