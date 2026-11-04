#ifndef BLANK_QUICK_PROFILE_RUNTIME_HPP // NOLINT(llvm-header-guard)
#define BLANK_QUICK_PROFILE_RUNTIME_HPP

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "minimal_profile.hpp"
#include "profile_storage.hpp"

namespace profile_runtime {
namespace detail {
struct session_state;
} // namespace detail

// NOLINTBEGIN(altera-struct-pack-align,readability-redundant-member-init)
struct profile_identity {
    std::string id{};
    std::string revision{};
    std::string generation{};
};

struct activation_snapshot {
    profile_payload::minimal_profile_snapshot profile{};
    std::vector<profile_payload::assembly_diagnostic> diagnostics{};
    bool requested_preserve_original_rules{};
    profile_identity identity{};
};

struct activation_result {
    std::shared_ptr<const activation_snapshot> value{};
    profile_payload::assembly_error error{profile_payload::assembly_error::none};
    std::vector<profile_payload::assembly_diagnostic> diagnostics{}; // Failure diagnostics.
};

// One immutable material generation. Copying a session shares its lazy in-memory cache.
// No network, storage calls or filesystem access occur in activate().
class profile_session {
  public:
    // Takes ownership of a coherent material snapshot; invalid limits throw invalid_argument.
    // NOLINTBEGIN(fuchsia-default-arguments-declarations)
    explicit profile_session(profile_storage::material_snapshot&& material,
                             const profile_payload::assembly_options& options = {});
    // NOLINTEND(fuchsia-default-arguments-declarations)

    // Default uses the saved preference. Explicit overrides are transient and never persisted.
    [[nodiscard]] auto activate() const noexcept -> activation_result;
    [[nodiscard]] auto activate(bool preserve_original_rules) const noexcept -> activation_result;
    [[nodiscard]] auto material() const noexcept
        -> std::shared_ptr<const profile_storage::material_snapshot>;

  private:
    std::shared_ptr<detail::session_state> state{};
};

enum class session_error : std::uint8_t {
    none,
    storage_failure,
    invalid_options,
    allocation_failure,
    unexpected_failure,
};

struct session_result {
    std::optional<profile_session> value{};
    session_error error{session_error::none};
    profile_storage::storage_error storage_error{profile_storage::storage_error::none};
};
// NOLINTEND(altera-struct-pack-align,readability-redundant-member-init)

// Preparation boundary: one locked material read, no download and no policy assembly.
// Call on a worker before activating or replacing the current session.
// NOLINTBEGIN(fuchsia-default-arguments-declarations)
[[nodiscard]] auto load_session(const profile_storage::profile_manager& manager,
                                std::string_view profile_id,
                                const profile_payload::assembly_options& options = {}) noexcept
    -> session_result;
// NOLINTEND(fuchsia-default-arguments-declarations)

} // namespace profile_runtime

#endif
