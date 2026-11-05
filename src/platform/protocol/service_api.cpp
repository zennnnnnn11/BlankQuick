#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QUrl>
#include <QUrlQuery>
#include <blank/protocol/service_api.hpp>
#include <blank/shared/timeouts.hpp>

int coreRequestTimeout(const QByteArray& path, const QByteArray& method,
                       int regularMilliseconds) noexcept {

    if (path == "/configs?force=true" && method == "PUT") {
        return static_cast<int>(timeout_budget::config_apply.count());
    }
    if (path == "/configs/geo" && method == "POST") {
        return static_cast<int>(timeout_budget::geo_update.count());
    }
    if (path.startsWith("/providers/") && method == "PUT") {
        return static_cast<int>(timeout_budget::provider_update.count());
    }
    if ((path == "/providers/proxies" || path.startsWith("/providers/proxies/")) &&
        method == "GET") {
        return static_cast<int>(timeout_budget::provider_read.count());
    }
    return regularMilliseconds;
}

QByteArray serviceResourceEndpoint(const QByteArray& path, const QByteArray& method,
                                   const QByteArray& body) {
    // 返回规范化后的可用端点，空值表示不准入。路径、方法、查询参数和正文
    // 一起判断，不能仅靠 startsWith 放行带有额外路径或重复参数的提权请求。
    if (path.size() > 8192) {
        return {};
    }
    const auto url = QUrl::fromEncoded(path, QUrl::StrictMode);
    if (!url.isValid() || !url.isRelative() || !url.authority().isEmpty() || url.hasFragment()) {
        return {};
    }
    if (method == "POST" && body.isEmpty() &&
        (path == "/cache/dns/flush" || path == "/cache/fakeip/flush")) {
        return path;
    }
    if (method == "PATCH" && path == "/rules/disable" && body.size() <= 8192) {
        const auto document = QJsonDocument::fromJson(body);
        const auto object = document.object();
        if (!document.isObject() || object.isEmpty() || object.size() > 128) {
            return {};
        }
        for (auto item = object.begin(); item != object.end(); ++item) {
            bool valid = false;
            const auto index = item.key().toInt(&valid);
            if (!valid || index < 0 || index >= 50000 || QString::number(index) != item.key() ||
                !item.value().isBool()) {
                return {};
            }
        }
        return path;
    }
    if (!body.isEmpty()) {
        return {};
    }
    const auto encoded = url.path(QUrl::FullyEncoded).toLatin1();
    const auto query = QUrlQuery(url).queryItems(QUrl::FullyDecoded);
    if (encoded == "/dns/query" && method == "GET" && query.size() == 2) {
        const QUrlQuery parameters(url);
        const auto domain = parameters.queryItemValue(QStringLiteral("name"), QUrl::FullyDecoded);
        const auto type = parameters.queryItemValue(QStringLiteral("type"), QUrl::FullyDecoded);
        const auto ace = QString::fromLatin1(QUrl::toAce(domain));
        static const QRegularExpression pattern(QStringLiteral(
            R"(^(?=.{1,253}\.?$)[a-zA-Z0-9](?:[a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?(?:\.[a-zA-Z0-9](?:[a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?)*\.?$)"));
        if (!pattern.match(ace).hasMatch() ||
            (type != QStringLiteral("A") && type != QStringLiteral("AAAA")) ||
            query.count({QStringLiteral("name"), domain}) != 1 ||
            query.count({QStringLiteral("type"), type}) != 1) {
            return {};
        }
        QUrlQuery normalized;
        normalized.addQueryItem(QStringLiteral("name"), ace);
        normalized.addQueryItem(QStringLiteral("type"), type);
        return "/dns/query?" + normalized.toString(QUrl::FullyEncoded).toUtf8();
    }
    const bool group = encoded.startsWith("/group/");
    const bool proxy = encoded.startsWith("/providers/proxies/");
    const bool rule = encoded.startsWith("/providers/rules/");
    if (method == "GET" && !url.hasQuery() &&
        (path == "/providers/proxies" || path == "/providers/rules")) {
        return path;
    }
    if (!group && !proxy && !rule) {
        return {};
    }
    QByteArray prefix("/providers/rules/");
    if (group) {
        prefix = "/group/";
    } else if (proxy) {
        prefix = "/providers/proxies/";
    }
    auto segment = encoded.sliced(prefix.size());
    const QByteArray suffix = group ? "/delay" : "/healthcheck";
    const bool action = method == "GET" && (group || proxy) && segment.endsWith(suffix);
    if (action) {
        segment.chop(suffix.size());
    }
    const auto name = QUrl::fromPercentEncoding(segment);
    if (name.isEmpty() || name.size() > 1024 || name.contains(QChar::Null) ||
        name == QStringLiteral(".") || name == QStringLiteral("..")) {
        return {};
    }
    auto endpoint = prefix + QUrl::toPercentEncoding(name);
    if (!group && method == "PUT" && !url.hasQuery()) {
        return endpoint;
    }
    if (proxy && action && !url.hasQuery()) {
        return endpoint + suffix;
    }
    if (!group || !action || query.size() != 2) {
        return {};
    }
    const QUrlQuery parameters(url);
    const auto target = parameters.queryItemValue(QStringLiteral("url"), QUrl::FullyDecoded);
    const QUrl destination(target, QUrl::StrictMode);
    const auto timeout = parameters.queryItemValue(QStringLiteral("timeout"));
    bool numeric = false;
    const int milliseconds = timeout.toInt(&numeric);
    if (!numeric || milliseconds < 1 || milliseconds > 5000 || target.size() > 2048 ||
        !destination.isValid() || destination.host().isEmpty() ||
        !destination.userInfo().isEmpty() || destination.hasFragment() ||
        (destination.scheme() != QStringLiteral("http") &&
         destination.scheme() != QStringLiteral("https")) ||
        query.count({QStringLiteral("url"), target}) != 1 ||
        query.count({QStringLiteral("timeout"), timeout}) != 1) {
        return {};
    }
    QUrlQuery normalized;
    normalized.addQueryItem(QStringLiteral("url"), destination.toString(QUrl::FullyEncoded));
    normalized.addQueryItem(QStringLiteral("timeout"), QString::number(milliseconds));
    return endpoint + suffix + '?' + normalized.toString(QUrl::FullyEncoded).toUtf8();
}

QByteArray serviceProxyEndpoint(const QByteArray& path, const QByteArray& method,
                                const QByteArray& body) {
    if (!path.startsWith("/proxies/") || path.size() > 8192) {
        return {};
    }
    const auto url = QUrl::fromEncoded(path, QUrl::StrictMode);
    if (!url.isValid() || !url.isRelative() || url.hasFragment()) {
        return {};
    }
    auto encoded = url.path(QUrl::FullyEncoded).sliced(9);
    const bool delay = method == "GET" && encoded.endsWith(QStringLiteral("/delay"));
    if (delay) {
        encoded.chop(6);
    }
    const auto name = QUrl::fromPercentEncoding(encoded.toLatin1());
    if (name.isEmpty() || name.size() > 1024 || name.contains(QChar::Null) ||
        name == QStringLiteral(".") || name == QStringLiteral("..")) {
        return {};
    }
    auto endpoint = "/proxies/" + QUrl::toPercentEncoding(name);
    const auto payload = QJsonDocument::fromJson(body).object();
    if (method == "PUT" && !url.hasQuery() && payload.size() == 1 &&
        payload.value(QStringLiteral("name")).isString() &&
        payload.value(QStringLiteral("name")).toString().size() <= 1024) {
        return endpoint;
    }
    if (method == "DELETE" && body.isEmpty() && !url.hasQuery()) {
        return endpoint;
    }
    if (method != "GET" || !delay || !body.isEmpty()) {
        return {};
    }
    const auto target = QUrlQuery(url).queryItemValue(QStringLiteral("url"), QUrl::FullyDecoded);
    const QUrl destination(target);
    if (target.size() > 2048 || !destination.isValid() || destination.host().isEmpty() ||
        (destination.scheme() != QStringLiteral("http") &&
         destination.scheme() != QStringLiteral("https"))) {
        return {};
    }
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("url"), destination.toString(QUrl::FullyEncoded));
    query.addQueryItem(QStringLiteral("timeout"), QStringLiteral("5000"));
    return endpoint + "/delay?" + query.toString(QUrl::FullyEncoded).toUtf8();
}
