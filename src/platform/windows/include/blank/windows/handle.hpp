#ifndef BLANK_WINDOWS_HANDLE_HPP
#define BLANK_WINDOWS_HANDLE_HPP

#include <memory>

namespace blank::windows {

struct HandleCloser {
    void operator()(void* value) const noexcept;
};

using Handle = std::unique_ptr<void, HandleCloser>;

struct LocalCloser {
    void operator()(void* value) const noexcept;
};

using LocalMemory = std::unique_ptr<void, LocalCloser>;

void check(bool success);
} // namespace blank::windows

#endif
