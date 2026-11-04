#ifndef BLANK_WINDOWS_HANDLE_HPP
#define BLANK_WINDOWS_HANDLE_HPP

#include <memory>

namespace blank::windows {
// Only owning Win32 HANDLEs belong here. Pseudo-handles and borrowed handles do not.
struct HandleCloser {
    void operator()(void* value) const noexcept;
};

using Handle = std::unique_ptr<void, HandleCloser>;

struct LocalCloser {
    void operator()(void* value) const noexcept;
};

using LocalMemory = std::unique_ptr<void, LocalCloser>;

// Call immediately after the failing Win32 API, before another call changes LastError.
void check(bool success);
} // namespace blank::windows

#endif
