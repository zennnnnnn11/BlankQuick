#include <blank/runtime_config/runtime_config.hpp>
#include <blank/runtime_config/settings_codec.hpp>
#include <blank/shared/json_document.hpp>
#include <concepts>
#include <exception>
#include <functional>
#include <new>
#include <type_traits>
#include <utility>

#include "runtime_config_detail.hpp"

namespace runtime_config {
namespace {
// NOLINTBEGIN(llvm-prefer-static-over-anonymous-namespace,fuchsia-default-arguments-calls)
template <std::default_initializable Value, std::invocable Operation>
    requires std::same_as<std::invoke_result_t<Operation>, Value>
[[nodiscard]] auto guarded_codec(Operation&& operation) noexcept -> settings_codec_result<Value> {
    try {
        return {.value = std::invoke(std::forward<Operation>(operation))};
    } catch (const detail::configuration_failure& failure) {
        return {.error = failure.code()};
    } catch (const json_detail::document::exception&) {
        return {.error = configuration_error::invalid_settings};
    } catch (const std::bad_alloc&) {
        return {.error = configuration_error::allocation_failure};
    } catch (const std::exception&) {
        return {.error = configuration_error::unexpected_failure};
    } catch (...) {
        return {.error = configuration_error::unexpected_failure};
    }
}
// NOLINTEND(llvm-prefer-static-over-anonymous-namespace,fuchsia-default-arguments-calls)
} // namespace

auto encode_settings(const client_settings& settings) noexcept
    -> settings_codec_result<json_detail::document> {
    return guarded_codec<json_detail::document>(
        [&]() -> json_detail::document { return detail::settings_document(settings); });
}

auto decode_settings(const json_detail::document& document) noexcept
    -> settings_codec_result<client_settings> {
    return guarded_codec<client_settings>(
        [&]() -> client_settings { return detail::settings_from_document(document); });
}
} // namespace runtime_config
