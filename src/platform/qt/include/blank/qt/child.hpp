#ifndef BLANK_QT_CHILD_HPP
#define BLANK_QT_CHILD_HPP

#include <QObject>
#include <concepts>
#include <memory>
#include <utility>

namespace blank::qt {
// Qt owns the child after setParent. The returned reference only observes it.
template <typename Child, typename... Arguments>
    requires std::derived_from<Child, QObject> && std::constructible_from<Child, Arguments...>
[[nodiscard]] Child& makeChild(QObject& parent, Arguments&&... arguments) {
    auto child = std::make_unique<Child>(std::forward<Arguments>(arguments)...);
    child->setParent(&parent);
    auto& observer = *child;
    static_cast<void>(child.release());
    return observer;
}
} // namespace blank::qt

#endif
