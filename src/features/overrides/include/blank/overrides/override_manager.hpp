#ifndef BLANK_QUICK_OVERRIDE_MANAGER_HPP // NOLINT(llvm-header-guard)
#define BLANK_QUICK_OVERRIDE_MANAGER_HPP

#include <blank/overrides/override_types.hpp>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace overrides {

inline constexpr std::chrono::milliseconds default_manager_lock_timeout{5000};
inline constexpr std::int64_t default_manager_item_limit{10000};
inline constexpr std::int64_t default_manager_index_bytes{16777216};
inline constexpr std::int64_t default_manager_content_bytes{2097152};
inline constexpr std::int64_t default_manager_selection_limit{128};
inline constexpr std::int64_t default_manager_loaded_bytes{33554432};

// NOLINTBEGIN(altera-struct-pack-align,readability-redundant-member-init)
struct manager_options {
    std::filesystem::path directory{}; // Absolute dedicated application data directory.
    std::chrono::milliseconds lock_timeout{default_manager_lock_timeout};
    std::int64_t max_items{default_manager_item_limit};
    std::int64_t max_bindings{default_manager_item_limit};
    std::int64_t max_index_bytes{default_manager_index_bytes};
    std::int64_t max_content_bytes{default_manager_content_bytes};
    std::int64_t max_selection{default_manager_selection_limit};
    std::int64_t max_loaded_bytes{default_manager_loaded_bytes};
};

struct override_summary {
    std::string id{};
    std::string name{};
    std::string ext{};
    bool is_global{};
    std::string revision{};
};

struct stored_override {
    override_item item{};
    std::string revision{};
};

struct profile_binding {
    std::string profile_id{};                // Opaque nonempty profile key, never used as a path.
    std::string revision{};                  // Empty only when no binding has been saved yet.
    std::vector<std::string> override_ids{}; // User-selected execution order.
};

struct selection_snapshot {
    profile_binding binding{};
    std::vector<stored_override> items{}; // Effective execution order, including globals.
};

struct cleanup_result {
    bool cleanup_pending{}; // Manifest committed; physical cleanup can be retried.
    std::int64_t removed_files{};
};
// NOLINTEND(altera-struct-pack-align,readability-redundant-member-init)

// Content versions are immutable; overrides.json is the atomic publication point.
// Constructors validate options without I/O. CRUD/binding calls serialize through
// an interprocess lock. No scripts execute while that lock is held.
class override_manager {
  public:
    explicit override_manager(manager_options selected);

    [[nodiscard]] auto list() const noexcept -> override_result<std::vector<override_summary>>;
    [[nodiscard]] auto get(std::string_view item_id) const noexcept
        -> override_result<stored_override>;
    // Empty id generates a UUID; a supplied canonical UUID must not already exist.
    [[nodiscard]] auto create(const override_item& item) const noexcept
        -> override_result<stored_override>;
    [[nodiscard]] auto update(const override_item& item,
                              std::string_view expected_revision) const noexcept
        -> override_result<stored_override>;
    [[nodiscard]] auto remove(std::string_view item_id,
                              std::string_view expected_revision) const noexcept
        -> override_result<cleanup_result>;
    [[nodiscard]] auto get_profile_binding(std::string_view profile_id) const noexcept
        -> override_result<profile_binding>;
    // Compare-and-swap against binding.revision; saving an empty selection retains
    // a revision tombstone so concurrent edits cannot resurrect stale selections.
    [[nodiscard]] auto set_profile_binding(const profile_binding& binding) const noexcept
        -> override_result<profile_binding>;
    // Globals in creation order, then selected IDs in binding order, each once.
    // All content is read from one locked manifest snapshot; no disk I/O in engine.
    [[nodiscard]] auto load_for_profile(std::string_view profile_id) const noexcept
        -> override_result<std::vector<override_item>>;
    [[nodiscard]] auto load_selection(std::string_view profile_id) const noexcept
        -> override_result<selection_snapshot>;
    [[nodiscard]] auto collect_garbage() const noexcept -> override_result<cleanup_result>;

  private:
    manager_options options;
};

} // namespace overrides

#endif
