#ifndef BLANK_QUICK_OVERRIDE_SCRIPT_HPP // NOLINT(llvm-header-guard)
#define BLANK_QUICK_OVERRIDE_SCRIPT_HPP

#include <blank/overrides/override_dom.hpp>

namespace overrides::script {
// Requires a QCoreApplication on the caller's process. Each invocation creates
// its own isolated interpreter and joins its watchdog before returning.
[[nodiscard]] auto evaluate(const dom::document& configuration, const override_item& patch,
                            const engine_options& options) noexcept -> dom::document_result;
} // namespace overrides::script

#endif
