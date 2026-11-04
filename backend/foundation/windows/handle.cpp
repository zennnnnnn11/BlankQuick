#include <Windows.h>

#include <blank/windows/handle.hpp>
#include <system_error>

namespace blank::windows
{
void HandleCloser::operator()(void* value) const noexcept
{
    if (value != nullptr && value != INVALID_HANDLE_VALUE)
    {
        CloseHandle(value);
    }
}

void LocalCloser::operator()(void* value) const noexcept
{
    LocalFree(value);
}

void check(bool success)
{
    if (!success)
    {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category());
    }
}
} // namespace blank::windows
