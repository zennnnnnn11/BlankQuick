#ifndef BLANK_QUICK_JSON_DOCUMENT_HPP // NOLINT(llvm-header-guard)
#define BLANK_QUICK_JSON_DOCUMENT_HPP

#include <nlohmann/json.hpp>

namespace json_detail {

using document = nlohmann::ordered_json;
}

#endif
