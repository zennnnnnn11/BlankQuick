#ifndef BLANK_QUICK_FILE_DURABILITY_HPP
#define BLANK_QUICK_FILE_DURABILITY_HPP

#include <QByteArray>
#include <QString>
#include <stop_token>

// published 与 error 不能合并成一个成功位：原子替换已经完成、目录刷盘失败时，
// 文件现场已经改变，但断电持久性未获确认，调用者不能按“完全没写入”处理。
struct DurableWrite {
    bool published = false;
    QString error;
};

[[nodiscard]] QString flushDirectory(const QString& directoryPath);
[[nodiscard]] DurableWrite writeDurableFile(const QString& path, const QByteArray& bytes,
                                            const std::stop_token& stop = {});

#endif
