#include <QtCore/QByteArray>
#include <QtCore/QCoreApplication>
#include <QtCore/QEventLoop>
#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtCore/QVariant>
#include <QtCore/QtTypes>
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
#include <blank/subscriptions/subscription_response.hpp>
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

// The adapter uses ordinary CPU loops and natural aggregate layout; optional
// local arguments and ISO string operations follow the surrounding modules.
// NOLINTBEGIN(llvm-prefer-static-over-anonymous-namespace,fuchsia-default-arguments-calls,fuchsia-default-arguments-declarations,altera-unroll-loops,altera-struct-pack-align,readability-redundant-member-init,abseil-string-find-str-contains)

using std::chrono::steady_clock;

// This final exception retains standard value semantics and never owns a resource.
class request_failure final : public std::exception { // clazy:exclude=copyable-polymorphic
  public:
    explicit request_failure(fetch_error selected) noexcept : error{selected} {}
    [[nodiscard]] auto code() const noexcept -> fetch_error {
        return error;
    }

  private:
    fetch_error error{};
};

constexpr auto first_printable_ascii = 32;
constexpr auto delete_character = 127;
constexpr auto maximum_url_bytes = 8192;
constexpr auto maximum_user_agent_bytes = 1024;
constexpr auto maximum_request_header_bytes = 65536;
constexpr auto maximum_request_header_fields = 128;
constexpr auto maximum_redirects = 20;
constexpr auto maximum_ca_bytes = 1048576;
constexpr auto maximum_port = 65535;
constexpr auto http_port = 80;
constexpr auto https_port = 443;
constexpr auto http_ok = 200;
constexpr auto response_buffer_bytes = 65536;
constexpr auto cancellation_poll_ms = 20;

[[noreturn]] auto fail(fetch_error error) -> void {
    throw request_failure{error};
}

[[nodiscard]] auto bytes(std::string_view text) -> QByteArray {
    // Qt receives the explicit length; the borrowed view need not be NUL-terminated.
    // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage)
    return QByteArray{text.data(), std::ssize(text)};
}
[[nodiscard]] auto qt_text(std::string_view text) -> QString {
    // Qt receives the explicit UTF-8 byte length, not a C string.
    // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage)
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
        return (octet < first_printable_ascii && character != '\t') || octet == delete_character;
    });
}

[[nodiscard]] auto http_url(std::string_view input) -> QUrl {
    if (input.empty() || std::ssize(input) > maximum_url_bytes || !clean_header_value(input) ||
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
    if (request.user_agent.empty() || std::ssize(request.user_agent) > maximum_user_agent_bytes ||
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
    auto remaining = std::int64_t{maximum_request_header_bytes};
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
    if (std::ssize(names) > maximum_request_header_fields ||
        std::ssize(request.if_none_match) > maximum_url_bytes ||
        std::ssize(request.if_modified_since) > maximum_url_bytes) {
        fail(fetch_error::invalid_request);
    }
}

auto validate_request(const fetch_request& request) -> void {
    constexpr auto max_timeout = std::chrono::hours{24};
    if (request.total_timeout.count() <= 0 || request.total_timeout > max_timeout ||
        request.route_timeout.count() <= 0 || request.route_timeout > max_timeout ||
        request.idle_timeout.count() <= 0 || request.idle_timeout > max_timeout ||
        request.routes.empty() || std::ssize(request.routes) > 4 || request.max_redirects < 0 ||
        request.max_redirects > maximum_redirects || request.max_body_bytes <= 0 ||
        request.max_body_bytes == std::numeric_limits<std::int64_t>::max() ||
        request.max_body_bytes > request.parser_limits.max_input_bytes ||
        std::ssize(request.extra_ca_pem) > maximum_ca_bytes) {
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
        if (route.host.empty() || route.port <= 0 || route.port > maximum_port ||
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
           left.port(left.scheme() == u"https" ? https_port : http_port) ==
               right.port(right.scheme() == u"https" ? https_port : http_port);
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
    const fetch_request* request{};
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
        if (status == http_ok) {
            response.body.append(chunk.constData(), static_cast<std::size_t>(chunk.size()));
        }
    }
}

[[nodiscard]] auto get_response(const QNetworkProxy& proxy, const request_context& context,
                                const QUrl& url) -> wire_response {

    auto manager = QNetworkAccessManager{nullptr};
    manager.setProxy(proxy);
    auto response = wire_response{};
    auto loop = QEventLoop{nullptr};
    auto timer = QTimer{nullptr};
    auto callback_error = std::exception_ptr{};

    auto* reply = manager.get(network_request(context, url));
    if (reply == nullptr) {
        fail(fetch_error::network_failure);
    }
    reply->setReadBufferSize(response_buffer_bytes);
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
    timer.start(std::chrono::milliseconds{cancellation_poll_ms});
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
        (response.status == 0 || response.status == http_ok)) {
        response.error = network_error(reply->error());
    }
    for (const auto& pair : reply->rawHeaderPairs()) {
        response.headers.push_back(
            {.name = pair.first.toStdString(), .value = pair.second.toStdString()});
    }
    response.redirect = reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();

    reply->deleteLater();
    return response;
}

[[nodiscard]] constexpr auto redirect_status(std::int64_t status) noexcept -> bool {
    constexpr auto moved_permanently = 301;
    constexpr auto found = 302;
    constexpr auto see_other = 303;
    constexpr auto temporary_redirect = 307;
    constexpr auto permanent_redirect = 308;
    return status == moved_permanently || status == found || status == see_other ||
           status == temporary_redirect || status == permanent_redirect;
}

[[nodiscard]] auto redirect_target(const QUrl& source, const QUrl& target) -> QUrl {
    // 每一跳都重新做 URL 准入，禁止 HTTPS 降级、嵌入用户信息和无效端口。
    // 相对跳转以当前地址解析；去掉片段后再参与循环检测，避免同页片段绕过上限。
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

auto finish_response(wire_response response, const fetch_request& request, fetch_result& result)
    -> void {
    // 网络层到此只交付已经收完的字节。内容解析不需要网络对象；本层额外保留
    // 最终 URL 和跳转次数，方便诊断，同时不把传输现场泄漏给数据算法。
    auto decoded = subscription_data::decode_response(
        {
            .body = std::move(response.body),
            .headers = std::move(response.headers),
            .status = response.status,
        },
        {.headers = request.response_header_limits, .parser = request.parser_limits});
    decoded.final_url = std::move(result.final_url);
    decoded.redirect_count = result.redirect_count;
    result = std::move(decoded);
}

[[nodiscard]] auto fetch_route(const request_context& context, const proxy_route& route)
    -> fetch_result {
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

[[nodiscard]] auto fetch_checked(const fetch_request& request, const std::stop_token& cancellation)
    -> fetch_result {
    // 路由按调用者提供的顺序尝试，共享总超时预算，每条路由还有独立上限。
    // 只对明确的传输故障换路由；TLS、正文语义或资源限制错误不能靠重试掩盖。
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
            {
                .request = &request,
                .initial_url = url,
                .authorization = authorization,
                .ssl = config,
                .deadline = std::min(deadline, steady_clock::now() + request.route_timeout),
                .cancellation = cancellation,
            },
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
// NOLINTEND(llvm-prefer-static-over-anonymous-namespace,fuchsia-default-arguments-calls,fuchsia-default-arguments-declarations,altera-unroll-loops,altera-struct-pack-align,readability-redundant-member-init,abseil-string-find-str-contains)

} // namespace

auto fetch_subscription(const fetch_request& request, const std::stop_token& cancellation) noexcept
    -> fetch_result {
    try {
        return fetch_checked(request, cancellation);
    } catch (const subscription_data::response_failure& failure) {
        return {.error = failure.code()};
    } catch (const request_failure& failure) {
        return {.error = failure.code()};
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
                          return fetch_subscription(request, cancellation);
                      });
}
} // namespace subscription_fetcher
