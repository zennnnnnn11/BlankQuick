#include <Windows.h>

#include <QDir>
#include <QJsonObject>
#include <blank/config/service_config.hpp>
#include <exception>

namespace {
QString checkProviderPaths(const QJsonObject& configuration, const QString& root) {
    // 数据层已检查字符串路径；这里只观察磁盘并拒绝重解析点。
    // 服务进程在自己的权限边界重复检查，不能信任客户端的检查结果。
    if (root.isEmpty()) {
        return {};
    }
    for (const auto* key : {"proxy-providers", "rule-providers"}) {
        const auto providers = configuration.value(QString::fromLatin1(key)).toObject();
        for (const auto& entry : providers) {
            const auto path = entry.toObject().value(QStringLiteral("path")).toString();
            if (path.isEmpty()) {
                continue;
            }
            auto current = root;
            const auto parts = QDir::fromNativeSeparators(path).split(u'/');
            for (const auto& part : parts) {
                if (part == QStringLiteral(".")) {
                    continue;
                }
                current += u'/' + part;
                const auto wide = QDir::toNativeSeparators(current).toStdWString();
                const auto attributes = GetFileAttributesW(wide.c_str());
                if (attributes != INVALID_FILE_ATTRIBUTES &&
                    (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                    return QStringLiteral("Provider path traverses a reparse point.");
                }
            }
        }
    }
    return {};
}
} // namespace

ServiceConfig compileServiceConfig(const QString& json, quint16 mixedPort, const QString& device,
                                   bool tunEnabled, const QString& providerRoot) try {
    auto result = composeServiceConfig(json, mixedPort, device, tunEnabled);
    if (!result.error.isEmpty()) {
        return result;
    }
    const auto error = checkProviderPaths(result.value, providerRoot);
    if (!error.isEmpty()) {
        return {.value = {}, .error = error};
    }
    return result;
} catch (const std::exception& error) {
    return {.value = {}, .error = QString::fromUtf8(error.what()).left(512)};
}
