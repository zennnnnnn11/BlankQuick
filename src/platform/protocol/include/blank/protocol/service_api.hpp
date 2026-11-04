#ifndef BLANK_QUICK_SERVICE_API_HPP
#define BLANK_QUICK_SERVICE_API_HPP

#include <QByteArray>

// Return a canonical, admitted proxy endpoint, or an empty array on invalid input.
[[nodiscard]] QByteArray serviceProxyEndpoint(const QByteArray& path, const QByteArray& method,
                                              const QByteArray& body);
[[nodiscard]] QByteArray serviceResourceEndpoint(const QByteArray& path, const QByteArray& method,
                                                 const QByteArray& body);
[[nodiscard]] int coreRequestTimeout(const QByteArray& path, const QByteArray& method,
                                     int regularMilliseconds) noexcept;

#endif
