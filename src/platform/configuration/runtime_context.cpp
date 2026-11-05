#include <QtCore/QRandomGenerator>

#include <blank/runtime_config/runtime_config.hpp>
#include <blank/runtime_config/runtime_context.hpp>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

// NOLINTBEGIN(llvm-prefer-static-over-anonymous-namespace,fuchsia-default-arguments-calls,altera-unroll-loops)
namespace runtime_config {
auto make_runtime_context(const std::filesystem::path& data_directory, std::int64_t controller_port)
    -> runtime_context {
    // 读取系统熵生成一次会话密钥，所以该函数位于平台层而非确定性生成器。
    // 这里只分配上下文值，不绑定端口、不创建目录；调用者仍负责真实执行环境的准入。
    constexpr std::int64_t maximum_port{65535};
    if (!data_directory.is_absolute() || data_directory == data_directory.root_path() ||
        controller_port < 1 || controller_port > maximum_port) {
        throw std::invalid_argument{"Invalid runtime context"};
    }
    constexpr std::string_view hex{"0123456789abcdef"};
    constexpr std::size_t secret_size{64};
    constexpr std::int64_t word_count{8};
    constexpr std::int64_t highest_nibble_shift{28};
    constexpr std::uint32_t nibble_mask{15U};
    auto secret = std::string{};
    secret.reserve(secret_size);
    for (std::int64_t index{}; index < word_count; ++index) {
        const auto value = QRandomGenerator::system()->generate();
        for (std::int64_t shift{highest_nibble_shift}; shift >= 0; shift -= 4) {
            secret.push_back(hex.at(static_cast<std::size_t>(
                (value >> static_cast<unsigned int>(shift)) & nibble_mask)));
        }
    }
    return {
        .data_directory = data_directory.lexically_normal(),
        .controller = "127.0.0.1:" + std::to_string(controller_port),
        .secret = std::move(secret),
    };
}
// NOLINTEND(llvm-prefer-static-over-anonymous-namespace,fuchsia-default-arguments-calls,altera-unroll-loops)
} // namespace runtime_config
