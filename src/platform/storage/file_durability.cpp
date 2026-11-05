#include <Windows.h>

#include <QDir>
#include <QFileInfo>
#include <QSaveFile>
#include <bit>
#include <blank/storage/file_durability.hpp>
#include <blank/windows/handle.hpp>
#include <io.h>
#include <system_error>

namespace {
QString windowsError(DWORD code) {
    return QStringLiteral("%1 (%2)")
        .arg(QString::fromStdString(std::system_category().message(static_cast<int>(code))))
        .arg(code);
}
} // namespace

QString flushDirectory(const QString& directoryPath) {
    if (directoryPath.isEmpty() || directoryPath.contains(QChar::Null)) {
        return QStringLiteral("目录刷盘需要有效的目录路径");
    }
    auto path = QDir::toNativeSeparators(QFileInfo(directoryPath).absoluteFilePath());
    if (!path.startsWith(QStringLiteral("\\\\?\\"))) {
        path = path.startsWith(QStringLiteral("\\\\"))
                   ? QStringLiteral("\\\\?\\UNC\\") + path.sliced(2)
                   : QStringLiteral("\\\\?\\") + path;
    }
    const auto wide = path.toStdWString();
    const blank::windows::Handle directory(CreateFileW(
        wide.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (directory.get() == INVALID_HANDLE_VALUE) {
        return QStringLiteral("无法打开目录进行刷盘：%1").arg(windowsError(GetLastError()));
    }
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (GetFileInformationByHandleEx(directory.get(), FileAttributeTagInfo, &attributes,
                                     sizeof(attributes)) == FALSE) {
        return QStringLiteral("无法核验刷盘目录：%1").arg(windowsError(GetLastError()));
    }
    if ((attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return QStringLiteral("拒绝将普通文件作为刷盘目录");
    }
    if (FlushFileBuffers(directory.get()) == FALSE) {
        return QStringLiteral("目录刷盘失败：%1").arg(windowsError(GetLastError()));
    }
    return {};
}

DurableWrite writeDurableFile(const QString& path, const QByteArray& bytes,
                              const std::stop_token& stop) {
    // 持久发布分三步：临时文件数据刷盘 -> 原子替换 -> 父目录刷盘。
    // 取消只在替换前生效；替换以后必须准确报告 published=true，即便最后一步失败。
    if (path.isEmpty() || path.contains(QChar::Null) || stop.stop_requested()) {
        return {
            .published = false,
            .error = QStringLiteral("文件路径无效或保存已取消"),
        };
    }
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.flush()) {
        return {
            .published = false,
            .error = QStringLiteral("无法写入临时文件：%1").arg(file.errorString()),
        };
    }
    const auto native = _get_osfhandle(file.handle());
    if (native == -1) {
        return {
            .published = false,
            .error = QStringLiteral("无法获取临时文件的原生句柄"),
        };
    }

    auto* const handle = std::bit_cast<HANDLE>(native);
    if (FlushFileBuffers(handle) == FALSE) {
        return {
            .published = false,
            .error = QStringLiteral("文件数据刷盘失败：%1").arg(windowsError(GetLastError())),
        };
    }
    if (stop.stop_requested()) {
        return {
            .published = false,
            .error = QStringLiteral("文件发布前保存已取消"),
        };
    }
    if (!file.commit()) {
        return {
            .published = false,
            .error = QStringLiteral("文件原子替换失败：%1").arg(file.errorString()),
        };
    }
    const auto error = flushDirectory(QFileInfo(path).absolutePath());
    return {
        .published = true,
        .error = error.isEmpty() ? QString{}
                                 : QStringLiteral("文件已替换，但未能确认目录持久化：") + error,
    };
}
