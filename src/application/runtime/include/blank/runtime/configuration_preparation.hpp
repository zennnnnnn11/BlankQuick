#ifndef BLANK_CONFIGURATION_PREPARATION_HPP
#define BLANK_CONFIGURATION_PREPARATION_HPP

#include <blank/process/native_kernel.hpp>
#include <blank/runtime/runtime_contract.hpp>
#include <stop_token>

struct ActivationCheck {
    QString payload;
    QString error;
    bool enableTun = false;
    TransactionStage stage = TransactionStage::Generation;
    TransactionError code = TransactionError::None;
    ActivationDiagnostics diagnostics;
};

// Cross-boundary preparation is an application use case: draft -> ordered
// patches/scripts -> final validation -> service admission -> native check.
[[nodiscard]] runtime_config::configuration_result
composeActivation(const ActivationInput& input, const runtime_config::runtime_context& context,
                  const runtime_config::generation_limits& limits = {}) noexcept;
[[nodiscard]] ActivationCheck prepareActivation(ActivationInput input,
                                                const runtime_config::runtime_context& context,
                                                const KernelOptions& options,
                                                const std::stop_token& stop);

#endif
