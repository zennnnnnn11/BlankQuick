#ifndef BLANK_QUICK_OVERRIDE_DETAIL_HPP // NOLINT(llvm-header-guard)
#define BLANK_QUICK_OVERRIDE_DETAIL_HPP

#include <blank/overrides/override_dom.hpp>
#include <blank/overrides/override_engine.hpp>
#include <blank/overrides/override_types.hpp>
#include <exception>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace overrides::detail {

using dom::document;

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

[[nodiscard]] auto valid_identifier(std::string_view value) noexcept -> bool;
[[nodiscard]] auto valid_utf8(std::string_view value) noexcept -> bool;
[[nodiscard]] auto valid_options(const engine_options& options) noexcept -> bool;
[[nodiscard]] auto parse_json(std::string_view text, const engine_options& options) -> document;
[[nodiscard]] auto parse_yaml(std::string_view text, const engine_options& options) -> document;
auto validate_document(const document& value, const engine_options& options) -> void;
auto merge_patch(document& target, const document& patch, const engine_options& options) -> void;

} // namespace overrides::detail

#endif
