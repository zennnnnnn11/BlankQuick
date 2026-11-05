#ifndef BLANK_QUICK_RUNTIME_CONTEXT_HPP // NOLINT(llvm-header-guard)
#define BLANK_QUICK_RUNTIME_CONTEXT_HPP

#include <blank/runtime_config/runtime_config.hpp>

namespace runtime_config {

// NOLINTBEGIN(altera-struct-pack-align,readability-redundant-member-init,fuchsia-default-arguments-declarations)
[[nodiscard]] auto make_runtime_context(const std::filesystem::path& data_directory,
                                        std::int64_t controller_port = default_controller_port)
    -> runtime_context;

// NOLINTEND(altera-struct-pack-align,readability-redundant-member-init,fuchsia-default-arguments-declarations)
} // namespace runtime_config

#endif
