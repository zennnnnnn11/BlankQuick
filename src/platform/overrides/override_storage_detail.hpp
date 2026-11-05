#ifndef BLANK_QUICK_OVERRIDE_STORAGE_DETAIL_HPP // NOLINT(llvm-header-guard)
#define BLANK_QUICK_OVERRIDE_STORAGE_DETAIL_HPP

#include <blank/overrides/override_dom.hpp>
#include <concepts>
#include <exception>
#include <filesystem>
#include <functional>
#include <new>
#include <utility>

namespace overrides::storage_detail {
// Exception values retain standard copy semantics; this final leaf is caught by const reference.
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

[[nodiscard]] inline auto parse_json(std::string_view text, const engine_options& options)
    -> dom::document {
    auto parsed = dom::read_document(text, options);
    if (parsed.error != override_error::none) {
        fail(parsed.error);
    }
    return std::move(parsed.value);
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
} // namespace overrides::storage_detail

#endif
