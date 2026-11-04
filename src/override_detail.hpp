#ifndef BLANK_QUICK_OVERRIDE_DETAIL_HPP // NOLINT(llvm-header-guard)
#define BLANK_QUICK_OVERRIDE_DETAIL_HPP

#include <concepts>
#include <exception>
#include <filesystem>
#include <functional>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "override_dom.hpp"
#include "override_engine.hpp"
#include "override_types.hpp"

namespace overrides::detail {

using dom::document;

// Exception transport is copyable; the final type is always caught by const reference.
// NOLINTNEXTLINE(altera-struct-pack-align)
class override_failure final : public std::exception { // clazy:exclude=copyable-polymorphic
  public:
    explicit override_failure(override_error selected) noexcept : error{selected} {}
    [[nodiscard]] auto code() const noexcept -> override_error {
        return error;
    }

  private:
    override_error error{};
};

[[noreturn]] inline auto fail(override_error error) -> void {
    throw override_failure{error};
}

template <std::default_initializable Value, typename Operation, typename... Arguments>
    requires std::invocable<Operation, Arguments...>
[[nodiscard]] auto guarded(Operation&& operation, Arguments&&... arguments) noexcept
    -> override_result<Value> {
    try {
        return {
            .value = std::invoke(std::forward<Operation>(operation),
                                 std::forward<Arguments>(arguments)...),
        };
    } catch (const override_failure& failure) {
        return {.error = failure.code()};
    } catch (const std::bad_alloc&) {
        return {.error = override_error::allocation_failure};
    } catch (const std::filesystem::filesystem_error&) {
        return {.error = override_error::io_failure};
    } catch (const std::exception&) {
        return {.error = override_error::unexpected_failure};
    } catch (...) {
        return {.error = override_error::unexpected_failure};
    }
}

[[nodiscard]] auto valid_identifier(std::string_view value) noexcept -> bool;
[[nodiscard]] auto valid_utf8(std::string_view value) noexcept -> bool;
[[nodiscard]] auto valid_options(const engine_options& options) noexcept -> bool;
[[nodiscard]] auto parse_json(std::string_view text, const engine_options& options) -> document;
[[nodiscard]] auto parse_yaml(std::string_view text, const engine_options& options) -> document;
auto validate_document(const document& value, const engine_options& options) -> void;
auto merge_patch(document& target, const document& patch, const engine_options& options) -> void;
[[nodiscard]] auto run_script(const document& configuration, const override_item& patch,
                              const engine_options& options, patch_report& report) -> document;

} // namespace overrides::detail

#endif
