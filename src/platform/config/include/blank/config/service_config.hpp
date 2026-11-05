#ifndef BLANK_QUICK_SERVICE_CONFIG_HPP
#define BLANK_QUICK_SERVICE_CONFIG_HPP

#include <blank/config/service_config_model.hpp>

// Physical admission adds reparse-point checks to the pure composition result.
// The service repeats admission in its own privilege boundary before execution.
[[nodiscard]] ServiceConfig compileServiceConfig(const QString& json, quint16 mixedPort,
                                                 const QString& device, bool tunEnabled = false,
                                                 const QString& providerRoot = {});
#endif
