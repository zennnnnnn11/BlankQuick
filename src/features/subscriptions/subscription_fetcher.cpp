#include <QtCore/QByteArray>
#include <QtCore/QCoreApplication>
#include <QtCore/QEventLoop>
#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtCore/QVariant>
#include <QtNetwork/QAuthenticator>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkProxy>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>
#include <QtNetwork/QSsl>
#include <QtNetwork/QSslCertificate>
#include <QtNetwork/QSslConfiguration>
#include <QtNetwork/QSslSocket>

#include <algorithm>
#include <array>
#include <blank/subscriptions/subscription_data.hpp>
#include <blank/subscriptions/subscription_fetcher.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <limits>
#include <new>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace subscription_fetcher {
namespace {
// Repository ISO C++20/default-argument/aggregate conventions, not Fuchsia/FPGA policy.
// NOLINTBEGIN(llvm-prefer-static-over-anonymous-namespace, fuchsia-default-arguments-calls,
// fuchsia-default-arguments-declarations, altera-unroll-loops, altera-struct-pack-align,
// abseil-string-find-str-contains, readability-redundant-member-init)

using steady_clock = std::chrono::steady_clock;

struct request_failure {
    fetch_error error{};
};

[[noreturn]] auto fail(fetch_error error) -> void {
    throw request_failure{error};
}

[[nodiscard]] auto bytes(std::string_view text) -> QByteArray {
    return QByteArray{text.data(), std::ssize(text)};
}
[[nodiscard]] auto qt_text(std::string_view text) -> QString {
    return QString::fromUtf8(text.data(), std::ssize(text));
}

[[nodiscard]] auto lower(std::string_view text) -> std::string {
    auto result = std::string{text};
    std::ranges::transform(result, result.begin(), [](char character) noexcept -> char {
        return character >= 'A' && character <= 'Z' ? static_cast<char>(character + ('a' - 'A'))
                                                    : character;
    });
    return result;
}

[[nodiscard]] constexpr auto clean_header_value(std::string_view value) noexcept -> bool {
    return std::ranges::none_of(value, [](char character) noexcept -> bool {
        const auto octet = static_cast<unsigned char>(character);
        return (octet < 32 && character != '\t') || octet == 127;
    });
}

[[nodiscard]] auto http_url(std::string_view input) -> QUrl {
    if (input.empty() || std::ssize(input) > 8192 || !clean_header_value(input) ||
        input.find('\t') != std::string_view::npos) {
        fail(fetch_error::invalid_request);
    }
    auto url = QUrl::fromEncoded(bytes(input), QUrl::StrictMode);
    if (!url.isValid() || url.host().isEmpty() ||
        (url.scheme() != u"http" && url.scheme() != u"https") || url.port() == 0) {
        fail(fetch_error::invalid_request);
    }
    url.setFragment(QString{});
    return url;
}

[[nodiscard]] auto valid_header_name(std::string_view name) noexcept -> bool {
    return !name.empty() && std::ranges::all_of(name, [](char character) noexcept -> bool {
        return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') ||
               std::string_view{"!#$%&'*+-.^_`|~"}.find(character) != std::string_view::npos;
    });
}

auto validate_request_headers(const fetch_request& request) -> void {
    if (request.user_agent.empty() || std::ssize(request.user_agent) > 1024 ||
        !clean_header_value(request.user_agent) || !clean_header_value(request.if_none_match) ||
        !clean_header_value(request.if_modified_since)) {
        fail(fetch_error::invalid_request);
    }
    constexpr auto reserved = std::to_array<std::string_view>({
        "host",
        "connection",
        "content-length",
        "transfer-encoding",
        "accept-encoding",
        "proxy-authorization",
        "user-agent",
        "if-none-match",
        "if-modified-since",
        "cookie",
        "set-cookie",
    });
    auto names = std::vector<std::string>{};
    auto remaining = std::int64_t{65536};
    for (const auto& header : request.headers) {
        const auto name = lower(header.name);
        if (!valid_header_name(header.name) || !clean_header_value(header.value) ||
            std::ranges::find(reserved, name) != reserved.end() ||
            std::ranges::find(names, name) != names.end() || std::ssize(header.name) > remaining) {
            fail(fetch_error::invalid_request);
        }
        remaining -= std::ssize(header.name);
        if (std::ssize(header.value) > remaining) {
            fail(fetch_error::invalid_request);
        }
        remaining -= std::ssize(header.value);
        names.push_back(name);
    }
    if (std::ssize(names) > 128 || std::ssize(request.if_none_match) > 8192 ||
        std::ssize(request.if_modified_since) > 8192) {
        fail(fetch_error::invalid_request);
    }
}

auto validate_request(const fetch_request& request) -> void {
    constexpr auto max_timeout = std::chrono::hours{24};
    if (request.total_timeout.count() <= 0 || request.total_timeout > max_timeout ||
        request.route_timeout.count() <= 0 || request.route_timeout > max_timeout ||
        request.idle_timeout.count() <= 0 || request.idle_timeout > max_timeout ||
        request.routes.empty() || std::ssize(request.routes) > 4 || request.max_redirects < 0 ||
        request.max_redirects > 20 || request.max_body_bytes <= 0 ||
        request.max_body_bytes == std::numeric_limits<std::int64_t>::max() ||
        request.max_body_bytes > request.parser_limits.max_input_bytes ||
        std::ssize(request.extra_ca_pem) > 1048576) {
        fail(fetch_error::invalid_request);
    }
    const auto& limits = request.response_header_limits;
    if (limits.max_fields <= 0 || limits.max_bytes <= 0 || limits.max_value_bytes <= 0) {
        fail(fetch_error::invalid_request);
    }
    validate_request_headers(request);
}

[[nodiscard]] auto network_proxy(const proxy_route& route) -> QNetworkProxy {
    switch (route.mode) {
    case proxy_mode::system:
        return QNetworkProxy{QNetworkProxy::DefaultProxy};
    case proxy_mode::direct:
        return QNetworkProxy{QNetworkProxy::NoProxy};
    case proxy_mode::http:
    case proxy_mode::socks5:
        if (route.host.empty() || route.port <= 0 || route.port > 65535 ||
            !clean_header_value(route.host) ||
            route.host.find_first_of("/\\ \t") != std::string::npos) {
            fail(fetch_error::invalid_request);
        }
        return QNetworkProxy{route.mode == proxy_mode::http ? QNetworkProxy::HttpProxy
                                                            : QNetworkProxy::Socks5Proxy,
                             qt_text(route.host), static_cast<quint16>(route.port),
                             qt_text(route.username), qt_text(route.password)};
    }
    fail(fetch_error::invalid_request);
}

[[nodiscard]] auto same_origin(const QUrl& left, const QUrl& right) -> bool {
    return left.scheme() == right.scheme() && left.host() == right.host() &&
           left.port(left.scheme() == u"https" ? 443 : 80) ==
               right.port(right.scheme() == u"https" ? 443 : 80);
}

[[nodiscard]] auto ssl_configuration(const fetch_request& request) -> QSslConfiguration {
    auto config = QSslConfiguration::defaultConfiguration();
    config.setPeerVerifyMode(QSslSocket::VerifyPeer);
    if (!request.extra_ca_pem.empty()) {
        const auto certificates = QSslCertificate::fromData(bytes(request.extra_ca_pem), QSsl::Pem);
        if (certificates.isEmpty()) {
            fail(fetch_error::invalid_request);
        }
        auto roots = config.caCertificates();
        roots.append(certificates);
        config.setCaCertificates(roots);
    }
    return config;
}

[[nodiscard]] auto basic_authentication(QUrl& url) -> QByteArray {
    if (url.userInfo().isEmpty()) {
        return {};
    }
    const auto user = url.userName(QUrl::FullyDecoded);
    if (user.contains(u':')) {
        fail(fetch_error::invalid_request);
    }
    const auto secret = (user + u':' + url.password(QUrl::FullyDecoded)).toUtf8();
    url.setUserInfo(QString{});
    return QByteArray{"Basic "} + secret.toBase64();
}

struct request_context {
    const fetch_request* request{}; // Observer; the synchronous caller owns the request.
    QUrl initial_url{};
    QByteArray authorization{};
    QSslConfiguration ssl{};
    steady_clock::time_point deadline{};
    std::stop_token cancellation{};
};

[[nodiscard]] auto network_request(const request_context& context, const QUrl& url)
    -> QNetworkRequest {
    const auto& options = (*context.request);
    auto request = QNetworkRequest{url};
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         QNetworkRequest::AlwaysNetwork);
    request.setTransferTimeout(options.idle_timeout);
    request.setSslConfiguration(context.ssl);
    request.setRawHeader(QByteArray{"User-Agent"}, bytes(options.user_agent));
    request.setRawHeader(QByteArray{"Accept"}, QByteArray{"*/*"});
    if (same_origin(context.initial_url, url)) {
        for (const auto& header : options.headers) {
            request.setRawHeader(bytes(header.name), bytes(header.value));
        }
        if (!context.authorization.isEmpty()) {
            if (request.hasRawHeader(QByteArray{"Authorization"})) {
                fail(fetch_error::invalid_request);
            }
            request.setRawHeader(QByteArray{"Authorization"}, context.authorization);
        }
        if (!options.if_none_match.empty()) {
            request.setRawHeader(QByteArray{"If-None-Match"}, bytes(options.if_none_match));
        }
        if (!options.if_modified_since.empty()) {
            request.setRawHeader(QByteArray{"If-Modified-Since"}, bytes(options.if_modified_since));
        }
    }
    return request;
}

[[nodiscard]] constexpr auto network_error(QNetworkReply::NetworkError error) noexcept
    -> fetch_error {
    constexpr auto mapping = std::to_array<std::pair<QNetworkReply::NetworkError, fetch_error>>({
        {QNetworkReply::NoError, fetch_error::none},
        {QNetworkReply::TimeoutError, fetch_error::timeout},
        {QNetworkReply::ProxyTimeoutError, fetch_error::timeout},
        {QNetworkReply::HostNotFoundError, fetch_error::dns_failure},
        {QNetworkReply::ConnectionRefusedError, fetch_error::connection_failure},
        {QNetworkReply::RemoteHostClosedError, fetch_error::connection_failure},
        {QNetworkReply::ProxyConnectionRefusedError, fetch_error::proxy_failure},
        {QNetworkReply::ProxyConnectionClosedError, fetch_error::proxy_failure},
        {QNetworkReply::ProxyNotFoundError, fetch_error::proxy_failure},
        {QNetworkReply::ProxyAuthenticationRequiredError, fetch_error::proxy_failure},
        {QNetworkReply::UnknownProxyError, fetch_error::proxy_failure},
        {QNetworkReply::SslHandshakeFailedError, fetch_error::tls_failure},
    });
    const auto found = std::ranges::find(mapping, error, &decltype(mapping)::value_type::first);
    return found == mapping.end() ? fetch_error::network_failure : found->second;
}

struct wire_response {
    std::string body{};
    std::vector<http_header> headers{};
    std::int64_t status{};
    fetch_error error{fetch_error::none};
    QUrl redirect{};
    std::int64_t received_bytes{};
};

[[nodiscard]] auto headers_within_limits(const QNetworkReply& reply, const header_limits& limits)
    -> bool {
    const auto& headers = reply.rawHeaderPairs();
    if (headers.size() > limits.max_fields) {
        return false;
    }
    auto remaining = limits.max_bytes;
    for (const auto& header : headers) {
        if (header.first.size() > remaining) {
            return false;
        }
        remaining -= header.first.size();
        if (header.second.size() > remaining || header.second.size() > limits.max_value_bytes) {
            return false;
        }
        remaining -= header.second.size();
    }
    return true;
}

auto consume_body(QNetworkReply& reply, wire_response& response, const fetch_request& request)
    -> void {
    if (!headers_within_limits(reply, request.response_header_limits)) {
        response.error = fetch_error::headers_too_large;
        reply.abort();
        return;
    }
    const auto status = reply.attribute(QNetworkRequest::HttpStatusCodeAttribute).toLongLong();
    constexpr std::int64_t chunk_size{65536};
    while (reply.bytesAvailable() > 0) {
        const auto remaining = request.max_body_bytes - response.received_bytes;
        const auto chunk = reply.read(std::min(chunk_size, remaining + 1));
        if (chunk.size() > remaining) {
            response.error = fetch_error::body_too_large;
            reply.abort();
            return;
        }
        response.received_bytes += chunk.size();
        if (status == 200) {
            response.body.append(chunk.constData(), static_cast<std::size_t>(chunk.size()));
        }
    }
}

[[nodiscard]] auto get_response(const QNetworkProxy& proxy, const request_context& context,
                                const QUrl& url) -> wire_response {
    // A fresh manager per hop confines origin authentication caches as well as
    // cookies. Qt's Manual auth flag also disables HTTP proxy challenges.
    auto manager = QNetworkAccessManager{nullptr};
    manager.setProxy(proxy);
    auto response = wire_response{};
    auto loop = QEventLoop{nullptr};
    auto timer = QTimer{nullptr};
    auto callback_error = std::exception_ptr{};
    // Observer only. QNetworkAccessManager owns the reply and frees it on exit.
    auto* reply = manager.get(network_request(context, url));
    if (reply == nullptr) {
        fail(fetch_error::network_failure);
    }
    reply->setReadBufferSize(65536);
    const auto credentials = manager.proxy();
    QObject::connect(&manager, &QNetworkAccessManager::proxyAuthenticationRequired, &loop,
                     [&, credentials, attempted = false](
                         const QNetworkProxy&, QAuthenticator* authenticator) mutable -> void {
                         if (authenticator == nullptr || attempted ||
                             (credentials.user().isEmpty() && credentials.password().isEmpty())) {
                             return;
                         }
                         attempted = true;
                         try {
                             authenticator->setUser(credentials.user());
                             authenticator->setPassword(credentials.password());
                         } catch (...) {
                             callback_error = std::current_exception();
                             loop.quit();
                         }
                     });

    const auto consume = [&]() -> void {
        try {
            consume_body(*reply, response, (*context.request));
        } catch (...) {
            callback_error = std::current_exception();
            loop.quit();
        }
    };
    QObject::connect(reply, &QNetworkReply::readyRead, &loop, consume);
    QObject::connect(reply, &QNetworkReply::metaDataChanged, &loop, consume);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timer, &QTimer::timeout, &loop, [&]() -> void {
        try {
            if (context.cancellation.stop_requested()) {
                response.error = fetch_error::cancelled;
                reply->abort();
            } else if (steady_clock::now() >= context.deadline) {
                response.error = fetch_error::timeout;
                reply->abort();
            }
        } catch (...) {
            callback_error = std::current_exception();
            loop.quit();
        }
    });
    timer.start(std::chrono::milliseconds{20});
    if (!reply->isFinished()) {
        static_cast<void>(loop.exec(QEventLoop::ExcludeUserInputEvents));
    }
    timer.stop();
    if (callback_error) {
        std::rethrow_exception(callback_error);
    }
    if (response.error != fetch_error::none) {
        return response;
    }
    consume_body(*reply, response, (*context.request));
    if (response.error != fetch_error::none) {
        return response;
    }
    response.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toLongLong();
    if (reply->error() != QNetworkReply::NoError &&
        (response.status == 0 || response.status == 200)) {
        response.error =
            response.status >= 400 ? fetch_error::http_status : network_error(reply->error());
    }
    for (const auto& pair : reply->rawHeaderPairs()) {
        response.headers.push_back({pair.first.toStdString(), pair.second.toStdString()});
    }
    response.redirect = reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
    // Release promptly after our context connections have been removed by loop's destruction.
    reply->deleteLater();
    return response;
}

[[nodiscard]] constexpr auto redirect_status(std::int64_t status) noexcept -> bool {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

[[nodiscard]] auto redirect_target(const QUrl& source, const QUrl& target) -> QUrl {
    if (target.isEmpty()) {
        fail(fetch_error::invalid_redirect);
    }
    auto next = source.resolved(target);
    if (!next.isValid() || next.host().isEmpty() || !next.userInfo().isEmpty() ||
        (next.scheme() != u"http" && next.scheme() != u"https") || next.port() == 0 ||
        (source.scheme() == u"https" && next.scheme() != u"https")) {
        fail(fetch_error::invalid_redirect);
    }
    next.setFragment(QString{});
    return next;
}

[[nodiscard]] auto looks_like_html(const wire_response& response) -> bool {
    const auto markup = lower(std::string_view{response.body}.substr(0, 512));
    const auto first = markup.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return false;
    }
    return markup.compare(first, 9, "<!doctype") == 0 || markup.compare(first, 5, "<html") == 0 ||
           markup.compare(first, 5, "<head") == 0;
}

auto finish_response(wire_response response, const fetch_request& request, fetch_result& result)
    -> void {
    result.http_status = response.status;
    result.metadata = parse_response_headers(response.headers, request.response_header_limits);
    if (result.metadata.error == metadata_error::allocation_failure) {
        fail(fetch_error::allocation_failure);
    }
    if (result.metadata.error == metadata_error::resource_limit) {
        fail(fetch_error::headers_too_large);
    }
    if (response.status == 304) {
        result.not_modified = true;
        return;
    }
    if (response.status != 200) {
        result.error = response.status == 204   ? fetch_error::empty_body
                       : response.status == 407 ? fetch_error::proxy_failure
                                                : fetch_error::http_status;
        return;
    }
    if (response.body.empty() ||
        std::ranges::all_of(response.body, [](char character) noexcept -> bool {
            return character == ' ' || character == '\t' || character == '\r' || character == '\n';
        })) {
        result.error = fetch_error::empty_body;
        return;
    }
    if (looks_like_html(response)) {
        result.error = fetch_error::unexpected_content;
        return;
    }
    result.subscription = node_input::parse_subscription(response.body, request.parser_limits);
    if (result.subscription.error == node_input::parse_error::allocation_failure) {
        result.error = fetch_error::allocation_failure;
    } else if (result.subscription.error != node_input::parse_error::none) {
        result.error = fetch_error::parse_failure;
    } else if (result.subscription.nodes.empty()) {
        result.error = fetch_error::no_supported_nodes;
    } else {
        result.raw_body = std::move(response.body);
    }
}

[[nodiscard]] auto fetch_route(request_context context, const proxy_route& route) -> fetch_result {
    auto result = fetch_result{};
    const auto proxy = network_proxy(route);
    auto current = context.initial_url;
    auto visited = std::vector<std::string>{};
    while (true) {
        if (context.cancellation.stop_requested()) {
            result.error = fetch_error::cancelled;
            return result;
        }
        if (steady_clock::now() >= context.deadline) {
            result.error = fetch_error::timeout;
            return result;
        }
        result.final_url = current.toEncoded().toStdString();
        if (std::ranges::find(visited, result.final_url) != visited.end()) {
            result.error = fetch_error::invalid_redirect;
            return result;
        }
        visited.push_back(result.final_url);
        auto response = get_response(proxy, context, current);
        result.http_status = response.status;
        if (response.error != fetch_error::none) {
            result.error = response.error;
            return result;
        }
        if (redirect_status(response.status)) {
            if (result.redirect_count >= (*context.request).max_redirects) {
                result.error = fetch_error::too_many_redirects;
                return result;
            }
            current = redirect_target(current, response.redirect);
            ++result.redirect_count;
            continue;
        }
        finish_response(std::move(response), (*context.request), result);
        return result;
    }
}

[[nodiscard]] constexpr auto retryable(fetch_error error) noexcept -> bool {
    return error == fetch_error::timeout || error == fetch_error::dns_failure ||
           error == fetch_error::connection_failure || error == fetch_error::proxy_failure ||
           error == fetch_error::network_failure;
}

[[nodiscard]] auto fetch_checked(const fetch_request& request, std::stop_token cancellation)
    -> fetch_result {
    validate_request(request);
    auto url = http_url(request.url);
    const auto authorization = basic_authentication(url);
    for (const auto& route : request.routes) {
        static_cast<void>(network_proxy(route));
    }
    if (QCoreApplication::instance() == nullptr) {
        fail(fetch_error::environment_unavailable);
    }
    if (cancellation.stop_requested()) {
        return {.error = fetch_error::cancelled};
    }
    const auto started = steady_clock::now();
    const auto deadline = started + request.total_timeout;
    const auto config = ssl_configuration(request);
    auto attempts = std::vector<fetch_attempt>{};
    auto result = fetch_result{};
    auto index = std::int64_t{};
    for (const auto& route : request.routes) {
        result = fetch_route(
            {.request = &request,
             .initial_url = url,
             .authorization = authorization,
             .ssl = config,
             .deadline = std::min(deadline, steady_clock::now() + request.route_timeout),
             .cancellation = cancellation},
            route);
        result.route_index = index;
        attempts.push_back(
            {.route_index = index, .error = result.error, .http_status = result.http_status});
        if (!retryable(result.error) || steady_clock::now() >= deadline) {
            break;
        }
        ++index;
    }
    result.attempts = std::move(attempts);
    return result;
}
// NOLINTEND(llvm-prefer-static-over-anonymous-namespace, fuchsia-default-arguments-calls,
// fuchsia-default-arguments-declarations, altera-unroll-loops, altera-struct-pack-align,
// abseil-string-find-str-contains, readability-redundant-member-init)
} // namespace

auto fetch_subscription(const fetch_request& request, std::stop_token cancellation) noexcept
    -> fetch_result {
    try {
        return fetch_checked(request, std::move(cancellation));
    } catch (const request_failure& failure) {
        return {.error = failure.error};
    } catch (const std::bad_alloc&) {
        return {.error = fetch_error::allocation_failure};
    } catch (const std::exception&) {
        return {.error = fetch_error::unexpected_failure};
    } catch (...) {
        return {.error = fetch_error::unexpected_failure};
    }
}

auto fetch_subscription_async(fetch_request request, std::stop_token cancellation)
    -> std::future<fetch_result> {
    return std::async(std::launch::async,
                      [request = std::move(request),
                       cancellation = std::move(cancellation)]() mutable noexcept -> fetch_result {
                          return fetch_subscription(request, std::move(cancellation));
                      });
}
} // namespace subscription_fetcher
