#include <array>
#include <blank/profiles/minimal_profile.hpp>
#include <blank/profiles/profile_runtime.hpp>
#include <blank/profiles/profile_storage.hpp>
#include <blank/profiles/profile_types.hpp>
#include <cstddef>
#include <exception>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

namespace profile_runtime::detail {

// NOLINTBEGIN(altera-struct-pack-align,readability-redundant-member-init)
struct session_state {
    // 会话绑定一个不可变的存储世代。两个槽位分别缓存“替换原规则”和
    // “保留原规则”的组装结果，不能只按 profile ID 共用一个缓存。
    std::shared_ptr<const profile_storage::material_snapshot> material{};
    profile_payload::assembly_options options{};
    std::mutex mutex{};
    std::array<std::shared_ptr<const activation_snapshot>, 2> cache{};
};
// NOLINTEND(altera-struct-pack-align,readability-redundant-member-init)

} // namespace profile_runtime::detail

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
    // 组装可能遍历大量节点，必须在锁外执行。锁只保护缓存槽位查找
    // 和发布；并发请求可以重复计算，最终共享同一份只读结果。
    auto options = state.options;
    options.preserve_original_rules = preserve;
    const auto pool = profile_payload::shared_node_pool{state.material, &state.material->nodes};
    // 别名 shared_ptr 指向快照里的节点数组，却共享整个 material 的所有权。
    // 返回的激活材料即使比 profile_session 活得更久，也不会悬空。
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
    // I/O 到这里结束。之后 activate() 只消费已加载的快照并缓存纯组装结果，
    // 不会为了重试生成而重新读取一个可能已被更新的 profile。
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
