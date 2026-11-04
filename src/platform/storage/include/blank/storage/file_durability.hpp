#ifndef BLANK_QUICK_FILE_DURABILITY_HPP
#define BLANK_QUICK_FILE_DURABILITY_HPP

#include <QByteArray>
#include <QString>
#include <stop_token>

struct DurableWrite {
    bool published = false;
    QString error;
};

[[nodiscard]] QString flushDirectory(const QString& directoryPath);
[[nodiscard]] DurableWrite writeDurableFile(const QString& path, const QByteArray& bytes,
                                            const std::stop_token& stop = {});

#endif
