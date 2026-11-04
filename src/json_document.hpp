#ifndef BLANK_QUICK_JSON_DOCUMENT_HPP // NOLINT(llvm-header-guard)
#define BLANK_QUICK_JSON_DOCUMENT_HPP

#include <nlohmann/json.hpp>

namespace json_detail {
// Shared representation for internal DOM integration; public worker APIs use standard types.
using document = nlohmann::ordered_json;
} // namespace json_detail

#endif
