#ifndef BLANK_RUNTIME_RULE_READBACK_HPP
#define BLANK_RUNTIME_RULE_READBACK_HPP

#include <QJsonArray>
#include <QJsonObject>
#include <optional>

namespace blank::runtime_detail {
[[nodiscard]] std::optional<QJsonArray> expectedRules(const QJsonObject& configuration);
[[nodiscard]] bool verifyRules(const QJsonArray& expected, const QJsonObject& response);
} // namespace blank::runtime_detail

#endif
