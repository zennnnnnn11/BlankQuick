#include <windows.h>

#include <blank/windows/handle.hpp>
#include <charconv>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <system_error>

namespace
{
template <typename Integer> bool parse(std::string_view text, Integer& value)
{
    const auto result =
        std::from_chars(std::to_address(text.begin()), std::to_address(text.end()), value);
    return result.ec == std::errc{} && result.ptr == std::to_address(text.end());
}
} // namespace

int main(int argc, char* argv[])
{
    if (argc != 3)
    {
        return 1;
    }
    const std::span arguments(argv, static_cast<std::size_t>(argc));
    DWORD pid = 0;
    std::uint64_t expectedCreation = 0;
    const auto parameters = arguments.subspan<1, 2>();
    if (!parse(std::string_view(parameters.front()), pid) ||
        !parse(std::string_view(parameters.back()), expectedCreation))
    {
        return 1;
    }
    const blank::windows::Handle process(
        OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid));
    if (process == nullptr)
    {
        return 2;
    }
    FILETIME created{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    const bool queried = GetProcessTimes(process.get(), &created, &exit, &kernel, &user) != FALSE;
    const auto actualCreation =
        (std::uint64_t{created.dwHighDateTime} << 32U) | created.dwLowDateTime;
    const bool alive = WaitForSingleObject(process.get(), 0) == WAIT_TIMEOUT;
    if (!queried || !alive || actualCreation != expectedCreation)
    {
        return 3;
    }
    FreeConsole(); // Also works when launched manually from a console during diagnosis.
    if (AttachConsole(pid) == FALSE)
    {
        return 4;
    }
    const bool signaled = SetConsoleCtrlHandler(nullptr, TRUE) != FALSE &&
                          GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0) != FALSE;
    FreeConsole();
    return signaled ? 0 : 5;
}
