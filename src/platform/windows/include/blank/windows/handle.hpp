#ifndef BLANK_WINDOWS_HANDLE_HPP
#define BLANK_WINDOWS_HANDLE_HPP

#include <memory>

namespace blank::windows {

struct HandleCloser {
    void operator()(void* value) const noexcept;
};

// Windows 句柄与 LocalAlloc 系列内存采用不同释放函数，不能混用两个别名。
// get() 只借出观察值；接入外部 API 的所有权转移集中在平台边界完成。
using Handle = std::unique_ptr<void, HandleCloser>;

struct LocalCloser {
    void operator()(void* value) const noexcept;
};

using LocalMemory = std::unique_ptr<void, LocalCloser>;

void check(bool success);
} // namespace blank::windows

#endif
