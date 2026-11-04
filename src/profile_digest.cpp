#include "profile_digest.hpp"

#include <QtCore/QByteArray>
#include <QtCore/QByteArrayView>
#include <QtCore/QCryptographicHash>

#include <Windows.h> // IWYU pragma: keep; bcrypt.h requires the Windows SDK declarations.
#include <algorithm>
#include <array>
#include <bcrypt.h>
#include <bit>
#include <cstddef>
#include <functional>
#include <iterator>
#include <memory>
#include <minwindef.h>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace profile_storage::detail {
namespace {
// ISO CPU loops do not use FPGA unroll directives.
// NOLINTBEGIN(llvm-prefer-static-over-anonymous-namespace,altera-unroll-loops)
constexpr int sha256_bytes{32};
constexpr int digest_block_bytes{8192};
using digest_bytes = std::array<unsigned char, sha256_bytes>;

auto release_hash(BCRYPT_HASH_HANDLE handle) noexcept -> void {
    static_cast<void>(BCryptDestroyHash(handle));
}

[[nodiscard]] auto native_sha256(std::string_view data) noexcept -> std::optional<digest_bytes> {
    BCRYPT_HASH_HANDLE raw{};
    const auto status = BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &raw, nullptr, 0, nullptr, 0, 0);
    if (status < 0 || raw == nullptr) {
        return std::nullopt;
    }
    const auto hash =
        std::unique_ptr<std::remove_pointer_t<BCRYPT_HASH_HANDLE>, decltype(&release_hash)>{
            raw, &release_hash};
    auto block = std::array<unsigned char, digest_block_bytes>{};
    static_assert(std::in_range<ULONG>(block.size()));
    static_assert(std::in_range<ULONG>(digest_bytes{}.size()));
    while (!data.empty()) {
        const auto count = std::min(std::ssize(data), std::ssize(block));
        const auto chunk = data.substr(0, static_cast<std::size_t>(count));
        std::ranges::transform(
            chunk, block.begin(),
            [](char character) noexcept -> unsigned char {
                return std::bit_cast<unsigned char>(character);
            },
            std::identity{});
        if (BCryptHashData(hash.get(), block.data(), static_cast<ULONG>(count), 0) < 0) {
            return std::nullopt;
        }
        data.remove_prefix(static_cast<std::size_t>(count));
    }
    auto result = digest_bytes{};
    if (BCryptFinishHash(hash.get(), result.data(), static_cast<ULONG>(result.size()), 0) < 0) {
        return std::nullopt;
    }
    return result;
}
// NOLINTEND(llvm-prefer-static-over-anonymous-namespace,altera-unroll-loops)
} // namespace

auto sha256(std::string_view data) -> std::string {
    if (const auto native = native_sha256(data)) {
        return QByteArrayView{*native}.toByteArray().toHex('\0').toStdString();
    }
    return QCryptographicHash::hash(QByteArrayView{data}, QCryptographicHash::Sha256)
        .toHex('\0')
        .toStdString();
}
} // namespace profile_storage::detail
