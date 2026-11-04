#include "profile_runtime.hpp"

#include <array>
#include <cstddef>
#include <exception>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

#include "minimal_profile.hpp"
#include "profile_storage.hpp"

namespace profile_runtime::detail {

// Independent state bundle; published material and cached activations are immutable.
// NOLINTBEGIN(altera-struct-pack-align,readability-redundant-member-init)
struct session_state {
    std::shared_ptr<const profile_storage::material_snapshot> material{};
    profile_payload::assembly_options options{};
    std::mutex mutex{};
    std::array<std::shared_ptr<const activation_snapshot>, 2> cache{};
};
// NOLINTEND(altera-struct-pack-align,readability-redundant-member-init)

} // namespace profile_runtime::detail

// Standard shared ownership and CPU alignment; callbacks use standard default options.
// NOLINTBEGIN(llvm-prefer-static-over-anonymous-namespace,fuchsia-default-arguments-calls,altera-struct-pack-align,readability-redundant-member-init)
namespace {

static_assert(std::is_nothrow_move_constructible_v<profile_storage::material_snapshot>);

[[nodiscard]] constexpr auto
valid_options(const profile_payload::assembly_options& options) noexcept -> bool {
    return options.max_raw_bytes > 0 && options.max_nodes > 0 && options.max_groups > 0 &&
           options.max_rules > 0 && options.max_rule_providers > 0 && options.max_references > 0;
}

[[nodiscard]] auto make_state(profile_storage::material_snapshot&& material,
                              const profile_payload::assembly_options& options)
    -> std::shared_ptr<profile_runtime::detail::session_state> {
    if (!valid_options(options)) {
        throw std::invalid_argument{"Invalid activation limits"};
    }
    // Allocate the state before transferring the material; failure leaves the input intact.
    auto state = std::make_shared<profile_runtime::detail::session_state>();
    state->options = options;
    state->material =
        std::make_shared<const profile_storage::material_snapshot>(std::move(material));
    return state;
}

[[nodiscard]] auto cached(profile_runtime::detail::session_state& state, std::size_t index)
    -> std::shared_ptr<const profile_runtime::activation_snapshot> {
    const auto lock = std::scoped_lock{state.mutex};
    return state.cache.at(index);
}

[[nodiscard]] auto assemble(profile_runtime::detail::session_state& state, bool preserve)
    -> profile_runtime::activation_result {
    const auto index = preserve ? std::size_t{1} : std::size_t{};
    if (auto previous = cached(state, index)) {
        return {.value = std::move(previous)};
    }
    auto options = state.options;
    options.preserve_original_rules = preserve;
    const auto pool = profile_payload::shared_node_pool{state.material, &state.material->nodes};
    auto assembled =
        profile_payload::try_assemble_minimal_profile(pool, state.material->raw, options);
    if (assembled.error != profile_payload::assembly_error::none) {
        return {.error = assembled.error, .diagnostics = std::move(assembled.diagnostics)};
    }
    auto candidate = std::make_shared<const profile_runtime::activation_snapshot>(
        profile_runtime::activation_snapshot{
            .profile = std::move(assembled.profile),
            .diagnostics = std::move(assembled.diagnostics),
            .requested_preserve_original_rules = preserve,
            .identity =
                {
                    .id = state.material->profile.id,
                    .revision = state.material->profile.revision,
                    .generation = state.material->profile.generation,
                },
        });
    // Expensive preparation stays outside the lock. Concurrent first activations
    // may prepare twice, but all readers receive the same published snapshot.
    const auto lock = std::scoped_lock{state.mutex};
    auto& slot = state.cache.at(index);
    if (!slot) {
        slot = std::move(candidate);
    }
    return {.value = slot};
}

} // namespace
// NOLINTEND(llvm-prefer-static-over-anonymous-namespace,fuchsia-default-arguments-calls,altera-struct-pack-align,readability-redundant-member-init)

namespace profile_runtime {

profile_session::profile_session(profile_storage::material_snapshot&& material,
                                 const profile_payload::assembly_options& options)
    : state{make_state(std::move(material), options)} {}

auto profile_session::material() const noexcept
    -> std::shared_ptr<const profile_storage::material_snapshot> {
    return state ? state->material : nullptr;
}

auto profile_session::activate() const noexcept -> activation_result {
    return state ? activate(state->material->profile.settings.preserve_original_rules)
                 : activation_result{.error = profile_payload::assembly_error::invalid_input};
}

auto profile_session::activate(bool preserve_original_rules) const noexcept -> activation_result {
    if (!state) {
        return {.error = profile_payload::assembly_error::invalid_input};
    }
    try {
        return assemble(*state, preserve_original_rules);
    } catch (const std::bad_alloc&) {
        return {.error = profile_payload::assembly_error::allocation_failure};
    } catch (const std::exception&) {
        return {.error = profile_payload::assembly_error::unexpected_failure};
    } catch (...) {
        return {.error = profile_payload::assembly_error::unexpected_failure};
    }
}

auto load_session(const profile_storage::profile_manager& manager, std::string_view profile_id,
                  const profile_payload::assembly_options& options) noexcept -> session_result {
    if (!valid_options(options)) {
        return {.error = session_error::invalid_options};
    }
    try {
        auto material = manager.load_material(profile_id);
        if (material.error != profile_storage::storage_error::none) {
            return {.error = session_error::storage_failure, .storage_error = material.error};
        }
        return {.value = profile_session{std::move(material.value), options}};
    } catch (const std::bad_alloc&) {
        return {.error = session_error::allocation_failure};
    } catch (const std::exception&) {
        return {.error = session_error::unexpected_failure};
    } catch (...) {
        return {.error = session_error::unexpected_failure};
    }
}

} // namespace profile_runtime
