#include <QtCore/QByteArray>
#include <QtCore/QString>
#include <QtCore/QUrl>

#include <algorithm>
#include <array>
#include <blank/subscriptions/subscription_account.hpp>
#include <blank/subscriptions/subscription_metadata.hpp>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace subscription_fetcher {
namespace {

// ISO value parsing intentionally uses ordinary loops, natural aggregate layout
// and standard strings rather than FPGA directives or an Abseil dependency.
// NOLINTBEGIN(llvm-prefer-static-over-anonymous-namespace,fuchsia-default-arguments-calls,fuchsia-default-arguments-declarations,altera-unroll-loops,altera-struct-pack-align,readability-redundant-member-init,abseil-string-find-str-contains)

[[nodiscard]] constexpr auto trim(std::string_view text) noexcept -> std::string_view {
    const auto first = text.find_first_not_of(" \t");
    return first == std::string_view::npos
               ? std::string_view{}
               : text.substr(first, text.find_last_not_of(" \t") - first + 1);
}

[[nodiscard]] auto lower(std::string_view text) -> std::string {
    auto result = std::string{text};
    std::ranges::transform(result, result.begin(), [](char character) noexcept -> char {
        return character >= 'A' && character <= 'Z' ? static_cast<char>(character + ('a' - 'A'))
                                                    : character;
    });
    return result;
}

[[nodiscard]] auto bytes(std::string_view text) -> QByteArray {
    // Qt receives the explicit byte count; no NUL terminator is required.
    // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage)
    return QByteArray{text.data(), std::ssize(text)};
}

[[nodiscard]] auto utf8_text(std::string_view text) -> bool {
    constexpr auto first_printable_ascii = 32;
    constexpr auto delete_character = 127;
    if (std::ranges::any_of(text, [](char character) noexcept -> bool {
            const auto octet = static_cast<unsigned char>(character);
            return octet < first_printable_ascii || octet == delete_character;
        })) {
        return false;
    }
    return bytes(text).isValidUtf8();
}

auto warn(metadata_result& result, metadata_field field, metadata_problem problem) -> void {
    result.warnings.push_back({.field = field, .problem = problem});
}

[[nodiscard]] auto unsigned_count(std::string_view text) noexcept -> std::optional<std::int64_t> {
    text = trim(text);
    if (text.empty() || text.front() < '0' || text.front() > '9') {
        return std::nullopt;
    }
    auto value = std::int64_t{};
    const auto parsed =
        std::from_chars(std::to_address(text.begin()), std::to_address(text.end()), value);
    if (parsed.ec != std::errc{} || parsed.ptr != std::to_address(text.end())) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] auto percent_bytes(std::string_view text) -> std::optional<std::string> {
    auto result = std::string{};
    for (auto index = std::int64_t{}; index < std::ssize(text); ++index) {
        const auto position = static_cast<std::size_t>(index);
        if (text.at(position) != '%') {
            result += text.at(position);
            continue;
        }
        if (index + 2 >= std::ssize(text)) {
            return std::nullopt;
        }
        const auto hex = text.substr(position + 1, 2);
        auto octet = int{};
        const auto parsed =
            std::from_chars(std::to_address(hex.begin()), std::to_address(hex.end()), octet, 16);
        if (parsed.ec != std::errc{} || parsed.ptr != std::to_address(hex.end())) {
            return std::nullopt;
        }
        result += static_cast<char>(octet);
        index += 2;
    }
    return result;
}

struct selected_header {
    std::optional<std::string_view> value{};
    int priority{};
};

[[nodiscard]] auto choose_header(std::span<const http_header> headers, std::string_view name,
                                 metadata_field field, metadata_result& result)
    -> std::optional<std::string_view> {
    auto selected = selected_header{};
    for (const auto& header : headers) {
        const auto key = lower(header.name);
        auto priority = int{};
        if (key == name) {
            priority = 2;
        } else if (key.size() > name.size() && key.ends_with(name) &&
                   key.at(key.size() - name.size() - 1) == '-') {
            priority = 1;
        }
        if (priority == 0) {
            continue;
        }
        if (selected.value.has_value()) {
            warn(result, field, metadata_problem::duplicate);
        }
        if (priority > selected.priority) {
            selected = {.value = trim(header.value), .priority = priority};
        }
    }
    return selected.value;
}

struct userinfo_parameter {
    std::string_view key{};
    std::string_view value{};
};

auto userinfo_count(metadata_result& result, const userinfo_parameter& parameter) -> void {
    struct count_field {
        std::string_view key{};
        metadata_field field{};
        std::optional<std::int64_t>* target{};
    };
    const auto fields = std::to_array<count_field>({
        {.key = "upload", .field = metadata_field::upload, .target = &result.account.upload_bytes},
        {
            .key = "download",
            .field = metadata_field::download,
            .target = &result.account.download_bytes,
        },
        {.key = "total", .field = metadata_field::total, .target = &result.account.total_bytes},
        {
            .key = "expire",
            .field = metadata_field::expire,
            .target = &result.account.expire_unix_seconds,
        },
    });
    const auto found = std::ranges::find(fields, parameter.key, &count_field::key);
    if (found == fields.end()) {
        return;
    }
    auto& target = *found->target;
    if (target.has_value()) {
        warn(result, found->field, metadata_problem::duplicate);
        return;
    }
    const auto parsed = unsigned_count(parameter.value);
    if (!parsed.has_value()) {
        warn(result, found->field, metadata_problem::out_of_range);
        return;
    }
    target = *parsed;
}

auto parse_userinfo(std::string_view text, metadata_result& result) -> void {
    while (!text.empty()) {
        const auto end = text.find(';');
        const auto pair = trim(text.substr(0, end));
        const auto equal = pair.find('=');
        if (equal == std::string_view::npos) {
            if (!pair.empty()) {
                warn(result, metadata_field::userinfo, metadata_problem::malformed);
            }
        } else {
            const auto key = lower(trim(pair.substr(0, equal)));
            userinfo_count(result, {.key = key, .value = trim(pair.substr(equal + 1))});
        }
        if (end == std::string_view::npos) {
            break;
        }
        text.remove_prefix(end + 1);
    }
}

struct disposition_parameter {
    std::string name{};
    std::string value{};
};

// Consume exactly one value and its delimiter. Quoted semicolons and escaped
// quotes belong to the value; an unterminated quote invalidates the entire header.
[[nodiscard]] auto disposition_value(std::string_view& text) -> std::optional<std::string> {
    if (!text.starts_with('"')) {
        const auto end = text.find(';');
        auto value = std::string{trim(text.substr(0, end))};
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        return value;
    }
    text.remove_prefix(1);
    auto value = std::string{};
    auto closed = false;
    while (!text.empty()) {
        const auto character = text.front();
        text.remove_prefix(1);
        if (character == '"') {
            closed = true;
            break;
        }
        if (character == '\\') {
            if (text.empty()) {
                return std::nullopt;
            }
            value += text.front();
            text.remove_prefix(1);
        } else {
            value += character;
        }
    }
    if (!closed) {
        return std::nullopt;
    }
    text = trim(text);
    if (!text.empty() && text.front() != ';') {
        return std::nullopt;
    }
    if (!text.empty()) {
        text.remove_prefix(1);
    }
    return value;
}

[[nodiscard]] auto disposition_parameters(std::string_view text)
    -> std::optional<std::vector<disposition_parameter>> {
    auto result = std::vector<disposition_parameter>{};
    const auto first = text.find(';');
    if (first == std::string_view::npos) {
        return result;
    }
    text.remove_prefix(first + 1);
    while (!trim(text).empty()) {
        text = trim(text);
        const auto equal = text.find('=');
        if (equal == std::string_view::npos) {
            return std::nullopt;
        }
        const auto name = lower(trim(text.substr(0, equal)));
        text = trim(text.substr(equal + 1));
        auto value = disposition_value(text);
        if (!value) {
            return std::nullopt;
        }
        result.push_back({.name = name, .value = std::move(*value)});
    }
    return result;
}

[[nodiscard]] auto extended_filename(std::string_view text) -> std::optional<std::string> {
    const auto first = text.find('\'');
    const auto second = first == std::string_view::npos ? first : text.find('\'', first + 1);
    if (second == std::string_view::npos) {
        return std::nullopt;
    }
    const auto charset = lower(text.substr(0, first));
    auto decoded = percent_bytes(text.substr(second + 1));
    if (!decoded.has_value()) {
        return std::nullopt;
    }
    if (charset == "utf-8" || charset == "utf8") {
        return decoded;
    }
    if (charset == "iso-8859-1") {
        return QString::fromLatin1(decoded->data(), std::ssize(*decoded)).toUtf8().toStdString();
    }
    return std::nullopt;
}

[[nodiscard]] auto title_from_filename(std::string filename) -> std::string {
    const auto suffix = filename.find_last_of('.');
    if (suffix == std::string::npos || suffix == 0) {
        return filename;
    }
    const auto extension = lower(filename.substr(suffix));
    if (extension == ".yaml" || extension == ".yml" || extension == ".json" ||
        extension == ".conf" || extension == ".txt") {
        filename.erase(suffix);
    }
    return filename;
}

auto parse_disposition(std::string_view text, metadata_result& result) -> void {
    const auto parameters = disposition_parameters(text);
    if (!parameters.has_value()) {
        warn(result, metadata_field::disposition, metadata_problem::malformed);
        return;
    }
    auto plain = std::optional<std::string>{};
    auto extended = std::optional<std::string>{};
    for (const auto& parameter : *parameters) {
        const auto is_extended = parameter.name == "filename*";
        if (!is_extended && parameter.name != "filename") {
            continue;
        }
        auto& target = is_extended ? extended : plain;
        if (target.has_value()) {
            warn(result, metadata_field::disposition, metadata_problem::duplicate);
        } else {
            target =
                is_extended ? extended_filename(parameter.value) : std::optional{parameter.value};
        }
        if (is_extended && !extended.has_value()) {
            warn(result, metadata_field::disposition, metadata_problem::unsupported_encoding);
        }
    }
    auto filename = extended.value_or(plain.value_or(""));
    const auto separator = filename.find_last_of("/\\");
    if (separator != std::string::npos) {
        filename.erase(0, separator + 1);
    }
    if (filename.empty() || filename == "." || filename == ".." || !utf8_text(filename)) {
        warn(result, metadata_field::disposition, metadata_problem::malformed);
        return;
    }
    result.account.suggested_filename = filename;
    if (!result.account.profile_name.has_value()) {
        result.account.profile_name = title_from_filename(std::move(filename));
    }
}

auto parse_title(std::string_view text, metadata_result& result) -> void {
    auto title = std::string{text};
    if (text.starts_with("base64:")) {
        const auto decoded = QByteArray::fromBase64Encoding(
            bytes(text.substr(7)), QByteArray::AbortOnBase64DecodingErrors);
        if (!decoded) {
            warn(result, metadata_field::title, metadata_problem::malformed);
            return;
        }
        title = decoded.decoded.toStdString();
    }
    if (title.empty() || !utf8_text(title)) {
        warn(result, metadata_field::title, metadata_problem::malformed);
        return;
    }
    result.account.profile_name = std::move(title);
}

auto parse_interval(std::string_view text, metadata_result& result) -> void {
    const auto hours = unsigned_count(text);
    constexpr std::int64_t seconds_per_hour{3600};
    if (!hours.has_value() || *hours == 0 ||
        *hours > std::numeric_limits<std::int64_t>::max() / seconds_per_hour) {
        warn(result, metadata_field::interval, metadata_problem::out_of_range);
        return;
    }
    result.account.update_interval = std::chrono::seconds{*hours * seconds_per_hour};
}

auto parse_home(std::string_view text, metadata_result& result) -> void {
    const auto url = QUrl::fromEncoded(bytes(text), QUrl::StrictMode);
    if (!url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty() ||
        (url.scheme() != u"http" && url.scheme() != u"https")) {
        warn(result, metadata_field::home_page, metadata_problem::malformed);
        return;
    }
    result.account.home_page_url = url.toEncoded().toStdString();
}

auto parse_metadata(std::span<const http_header> headers, metadata_result& result) -> void {
    if (const auto value =
            choose_header(headers, "subscription-userinfo", metadata_field::userinfo, result)) {
        parse_userinfo(*value, result);
    }
    if (const auto value = choose_header(headers, "profile-title", metadata_field::title, result)) {
        parse_title(*value, result);
    }
    if (const auto value =
            choose_header(headers, "content-disposition", metadata_field::disposition, result)) {
        parse_disposition(*value, result);
    }
    if (const auto value =
            choose_header(headers, "profile-update-interval", metadata_field::interval, result)) {
        parse_interval(*value, result);
    }
    if (const auto value =
            choose_header(headers, "profile-web-page-url", metadata_field::home_page, result)) {
        parse_home(*value, result);
    }
    if (const auto value = choose_header(headers, "plan-name", metadata_field::plan, result)) {
        if (!value->empty() && utf8_text(*value)) {
            result.account.plan_name = std::string{*value};
        } else {
            warn(result, metadata_field::plan, metadata_problem::malformed);
        }
    }
    if (const auto value = choose_header(headers, "etag", metadata_field::etag, result)) {
        result.account.etag = std::string{*value};
    }
    if (const auto value =
            choose_header(headers, "last-modified", metadata_field::last_modified, result)) {
        result.account.last_modified = std::string{*value};
    }
}

[[nodiscard]] auto valid_headers(std::span<const http_header> headers,
                                 const header_limits& limits) noexcept -> bool {
    if (limits.max_fields <= 0 || limits.max_bytes <= 0 || limits.max_value_bytes <= 0 ||
        std::ssize(headers) > limits.max_fields) {
        return false;
    }
    auto remaining = limits.max_bytes;
    for (const auto& header : headers) {
        if (std::ssize(header.name) > remaining) {
            return false;
        }
        remaining -= std::ssize(header.name);
        if (std::ssize(header.value) > remaining ||
            std::ssize(header.value) > limits.max_value_bytes) {
            return false;
        }
        remaining -= std::ssize(header.value);
        if (header.name.find_first_of("\r\n") != std::string::npos ||
            header.value.find_first_of("\r\n") != std::string::npos ||
            header.name.find('\0') != std::string::npos ||
            header.value.find('\0') != std::string::npos) {
            return false;
        }
    }
    return true;
}
// NOLINTEND(llvm-prefer-static-over-anonymous-namespace,fuchsia-default-arguments-calls,fuchsia-default-arguments-declarations,altera-unroll-loops,altera-struct-pack-align,readability-redundant-member-init,abseil-string-find-str-contains)

} // namespace

auto parse_response_headers(std::span<const http_header> headers,
                            const header_limits& limits) noexcept -> metadata_result {
    // 原始头部先统一检查总量、单值长度和控制字符，再解析各业务字段。
    // 字段格式问题由 metadata 的诊断描述；不应因一个无效标题就丢掉合法节点正文。
    auto result = metadata_result{};
    try {
        if (!valid_headers(headers, limits)) {
            result.error = metadata_error::resource_limit;
            return result;
        }
        parse_metadata(headers, result);
    } catch (const std::bad_alloc&) {
        result.error = metadata_error::allocation_failure;
    } catch (const std::exception&) {
        result.error = metadata_error::unexpected_failure;
    } catch (...) {
        result.error = metadata_error::unexpected_failure;
    }
    return result;
}
} // namespace subscription_fetcher
