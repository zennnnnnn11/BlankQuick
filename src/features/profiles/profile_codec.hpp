#ifndef BLANK_QUICK_PROFILE_CODEC_HPP // NOLINT(llvm-header-guard)
#define BLANK_QUICK_PROFILE_CODEC_HPP

#include <blank/profiles/profile_storage.hpp>
#include <exception>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace profile_storage::detail {

// NOLINTNEXTLINE(altera-struct-pack-align)
class storage_failure final : public std::exception {
  public:
    explicit storage_failure(storage_error selected) noexcept : error{selected} {}
    [[nodiscard]] auto code() const noexcept -> storage_error {
        return error;
    }

  private:
    storage_error error{};
};

[[nodiscard]] auto encode_index(std::span<const profile_record> records) -> std::string;
[[nodiscard]] auto decode_index(std::string_view text, std::int64_t max_profiles)
    -> std::vector<profile_record>;
[[nodiscard]] auto encode_nodes(std::span<const node_model::unified_node> nodes) -> std::string;
[[nodiscard]] auto decode_nodes(std::string_view text, std::int64_t max_nodes)
    -> std::vector<node_model::unified_node>;

} // namespace profile_storage::detail

#endif
