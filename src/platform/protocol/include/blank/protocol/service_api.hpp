#ifndef BLANK_QUICK_SERVICE_API_HPP
#define BLANK_QUICK_SERVICE_API_HPP

#include <QByteArray>

[[nodiscard]] QByteArray serviceProxyEndpoint(const QByteArray& path, const QByteArray& method,
                                              const QByteArray& body);
[[nodiscard]] QByteArray serviceResourceEndpoint(const QByteArray& path, const QByteArray& method,
                                                 const QByteArray& body);
[[nodiscard]] int coreRequestTimeout(const QByteArray& path, const QByteArray& method,
                                     int regularMilliseconds) noexcept;

#endif
