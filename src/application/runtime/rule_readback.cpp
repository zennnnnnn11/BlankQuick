#include "rule_readback.hpp"

#include <QHostAddress>
#include <QStringList>
#include <algorithm>
#include <array>
#include <string_view>
#include <utility>

namespace {
using namespace Qt::Literals::StringLiterals;

constexpr auto ruleTypes = std::to_array<std::pair<std::string_view, std::string_view>>({
    {"MATCH", "Match"},
    {"DOMAIN", "Domain"},
    {"DOMAIN-SUFFIX", "DomainSuffix"},
    {"DOMAIN-KEYWORD", "DomainKeyword"},
    {"DOMAIN-REGEX", "DomainRegex"},
    {"DOMAIN-WILDCARD", "DomainWildcard"},
    {"GEOSITE", "GeoSite"},
    {"GEOIP", "GeoIP"},
    {"SRC-GEOIP", "SrcGeoIP"},
    {"IP-ASN", "IPASN"},
    {"SRC-IP-ASN", "SrcIPASN"},
    {"IP-CIDR", "IPCIDR"},
    {"IP-CIDR6", "IPCIDR"},
    {"SRC-IP-CIDR", "SrcIPCIDR"},
    {"IP-SUFFIX", "IPSuffix"},
    {"SRC-IP-SUFFIX", "SrcIPSuffix"},
    {"SRC-PORT", "SrcPort"},
    {"DST-PORT", "DstPort"},
    {"IN-PORT", "InPort"},
    {"DSCP", "DSCP"},
    {"IN-USER", "InUser"},
    {"IN-NAME", "InName"},
    {"IN-TYPE", "InType"},
    {"PROCESS-NAME", "ProcessName"},
    {"PROCESS-PATH", "ProcessPath"},
    {"PROCESS-NAME-REGEX", "ProcessNameRegex"},
    {"PROCESS-PATH-REGEX", "ProcessPathRegex"},
    {"PROCESS-NAME-WILDCARD", "ProcessNameWildcard"},
    {"PROCESS-PATH-WILDCARD", "ProcessPathWildcard"},
    {"REMATCH-NAME", "RematchName"},
    {"RULE-SET", "RuleSet"},
    {"NETWORK", "Network"},
    {"UID", "Uid"},
});

[[nodiscard]] std::optional<QString> prefixPayload(const QString& payload) {
    const auto slash = payload.lastIndexOf(u'/');
    if (slash < 0) {
        return std::nullopt;
    }
    const QHostAddress address{payload.first(slash)};
    auto valid = false;
    const auto bits = payload.sliced(slash + 1).toInt(&valid);
    const auto maximum = address.protocol() == QAbstractSocket::IPv4Protocol ? 32 : 128;
    if (!valid || address.isNull() || !address.scopeId().isEmpty() || bits < 0 || bits > maximum) {
        return std::nullopt;
    }
    return address.toString().toLower() + u'/' + QString::number(bits);
}

[[nodiscard]] std::optional<QJsonObject> expectedRule(const QString& text) {
    auto parts = text.split(u',');
    for (auto& part : parts) {
        part = part.trimmed();
    }
    const auto type = parts.front().toUpper().toStdString();
    const auto mapping =
        std::ranges::find(ruleTypes, type, &decltype(ruleTypes)::value_type::first);
    if (mapping == ruleTypes.end()) {
        return std::nullopt;
    }
    auto runtimeType = QString::fromUtf8(mapping->second);
    if (type == "MATCH") {
        if (parts.size() != 2 || parts.at(1).isEmpty()) {
            return std::nullopt;
        }
        return QJsonObject{
            {"type"_L1, runtimeType},
            {"payload"_L1, QString{}},
            {"proxy"_L1, parts.at(1)},
        };
    }
    if (parts.size() < 3) {
        return std::nullopt;
    }
    const auto regex =
        type == "DOMAIN-REGEX" || type == "PROCESS-NAME-REGEX" || type == "PROCESS-PATH-REGEX";
    auto payload = regex ? parts.sliced(1, parts.size() - 2).join(u',') : parts.at(1);
    const auto target = regex ? parts.back() : parts.at(2);
    if (!regex && parts.size() > 3) {
        const auto ipRule = type == "GEOIP" || type == "IP-ASN" || type == "IP-CIDR" ||
                            type == "IP-CIDR6" || type == "IP-SUFFIX" || type == "RULE-SET";
        if (!ipRule) {
            return std::nullopt;
        }
        const auto parameters = parts.sliced(3);
        for (const auto& parameter : parameters) {
            if (parameter == "src"_L1) {
                // RuleSet keeps its type in source mode; IP rules expose Src* types.
                if (type != "RULE-SET" && !runtimeType.startsWith("Src"_L1)) {
                    runtimeType.prepend("Src"_L1);
                }
            } else if (parameter != "no-resolve"_L1) {
                return std::nullopt;
            }
        }
    }
    if (payload.isEmpty() || target.isEmpty()) {
        return std::nullopt;
    }
    if (type == "DOMAIN" || type == "DOMAIN-SUFFIX" || type == "DOMAIN-KEYWORD" ||
        type == "DOMAIN-WILDCARD" || type == "GEOIP" || type == "SRC-GEOIP" || type == "NETWORK") {
        payload = payload.toLower();
    } else if (type == "IN-TYPE") {
        payload = payload.toUpper();
    } else if (type == "IP-CIDR" || type == "IP-CIDR6" || type == "SRC-IP-CIDR") {
        const auto prefix = prefixPayload(payload);
        if (!prefix) {
            return std::nullopt;
        }
        payload = *prefix;
    }
    return QJsonObject{{"type"_L1, runtimeType}, {"payload"_L1, payload}, {"proxy"_L1, target}};
}
} // namespace

namespace blank::runtime_detail {
std::optional<QJsonArray> expectedRules(const QJsonObject& configuration) {
    const auto subrules = configuration.value("sub-rules"_L1);
    const auto rules = configuration.value("rules"_L1);
    if ((!subrules.isUndefined() && (!subrules.isObject() || !subrules.toObject().isEmpty())) ||
        (!rules.isUndefined() && !rules.isArray())) {
        return std::nullopt;
    }
    auto result = QJsonArray{};
    const auto entries = rules.toArray();
    for (const auto& entry : entries) {
        if (!entry.isString()) {
            return std::nullopt;
        }
        const auto rule = expectedRule(entry.toString());
        if (!rule) {
            return std::nullopt;
        }
        result.push_back(*rule);
    }
    return result;
}

bool verifyRules(const QJsonArray& expected, const QJsonObject& response) {
    const auto value = response.value("rules"_L1);
    if (!value.isArray() || value.toArray().size() != expected.size()) {
        return false;
    }
    const auto actual = value.toArray();
    for (qsizetype index = 0; index < expected.size(); ++index) {
        const auto rule = actual.at(index).toObject();
        const auto extra = rule.value("extra"_L1).toObject();
        if (rule.value("index"_L1).toInteger(-1) != index ||
            extra.value("disabled"_L1) != QJsonValue{false}) {
            return false;
        }
        const auto definition = expected.at(index).toObject();
        for (auto field = definition.begin(); field != definition.end(); ++field) {
            if (rule.value(field.key()) != field.value()) {
                return false;
            }
        }
    }
    return true;
}
} // namespace blank::runtime_detail
