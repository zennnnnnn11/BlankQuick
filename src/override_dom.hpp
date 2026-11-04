#ifndef BLANK_QUICK_OVERRIDE_DOM_HPP // NOLINT(llvm-header-guard)
#define BLANK_QUICK_OVERRIDE_DOM_HPP

#include <cstdint>
#include <span>
#include <vector>

#include "json_document.hpp"
#include "override_engine.hpp"
#include "override_types.hpp"

// Narrow internal integration contract. No parser, script or storage implementation escapes.
namespace overrides::dom {
using json_detail::document;

enum class failure_stage : std::int8_t { none, input, patch };

// Independent result fields use natural alignment and explicit value initialization.
// NOLINTBEGIN(altera-struct-pack-align,readability-redundant-member-init)
struct document_result {
    document value{}; // NOLINT(fuchsia-default-arguments-calls)
    std::vector<patch_report> reports{};
    override_error error{};
    failure_stage stage{failure_stage::none};
};
// NOLINTEND(altera-struct-pack-align,readability-redundant-member-init)

// Failure discards the candidate. stage identifies the failure independently of report count.
[[nodiscard]] auto apply_document(document candidate, std::span<const override_item> patches,
                                  const engine_options& options) noexcept -> document_result;
} // namespace overrides::dom

#endif
