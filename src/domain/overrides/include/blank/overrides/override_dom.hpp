#ifndef BLANK_QUICK_OVERRIDE_DOM_HPP // NOLINT(llvm-header-guard)
#define BLANK_QUICK_OVERRIDE_DOM_HPP

#include <blank/overrides/override_engine.hpp>
#include <blank/overrides/override_types.hpp>
#include <blank/shared/json_document.hpp>
#include <cstdint>
#include <span>
#include <vector>

namespace overrides::dom {
using json_detail::document;

enum class failure_stage : std::int8_t { none, input, patch };

// NOLINTBEGIN(altera-struct-pack-align,readability-redundant-member-init)
struct document_result {
    document value{}; // NOLINT(fuchsia-default-arguments-calls)
    std::vector<patch_report> reports{};
    override_error error{};
    failure_stage stage{failure_stage::none};
};
// NOLINTEND(altera-struct-pack-align,readability-redundant-member-init)

[[nodiscard]] auto apply_document(document candidate, std::span<const override_item> patches,
                                  const engine_options& options) noexcept -> document_result;

// All documents entering from a script or storage pass the same byte, depth and
// value budgets. Callers receive data/errors rather than the parser's exceptions.
[[nodiscard]] auto read_document(std::string_view text, const engine_options& options) noexcept
    -> document_result;
[[nodiscard]] auto valid_identifier(std::string_view value) noexcept -> bool;
[[nodiscard]] auto valid_utf8(std::string_view value) noexcept -> bool;
} // namespace overrides::dom

#endif
