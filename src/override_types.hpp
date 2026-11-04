#ifndef BLANK_QUICK_OVERRIDE_TYPES_HPP // NOLINT(llvm-header-guard)
#define BLANK_QUICK_OVERRIDE_TYPES_HPP

#include <concepts>
#include <cstdint>
#include <string>

namespace overrides {

enum class override_error : std::int8_t {
    none,
    invalid_input,
    invalid_document,
    invalid_operation,
    script_error,
    invalid_script_result,
    timeout,
    application_required,
    resource_limit,
    not_found,
    conflict,
    busy,
    unsafe_path,
    io_failure,
    corrupt_index,
    corrupt_content,
    unsupported_version,
    allocation_failure,
    unexpected_failure,
};

// NOLINTBEGIN(altera-struct-pack-align,readability-redundant-member-init)
struct override_item {
    std::string id{}; // Canonical lowercase UUID, 32 hexadecimal digits.
    std::string name{};
    std::string ext{}; // "yaml" or "js".
    bool is_global{};
    std::string content{};
};

template <std::default_initializable Value> struct override_result {
    Value value{};
    override_error error{override_error::none};
};
// NOLINTEND(altera-struct-pack-align,readability-redundant-member-init)

} // namespace overrides

#endif
