#include "dvv/base/uri.h"

#include <cstddef>
#include <expected>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dvv {

namespace {

// ASCII-only character classes: independent of the C locale.
bool isAlpha(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool isAlnum(char c) noexcept {
    return isAlpha(c) || (c >= '0' && c <= '9');
}

char toLower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool equalsIgnoreCase(std::string_view text, std::string_view lower) noexcept {
    if (text.size() != lower.size()) {
        return false;
    }
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (toLower(text[i]) != lower[i]) {
            return false;
        }
    }
    return true;
}

bool isHierarchical(std::string_view scheme) noexcept {
    return scheme == "file" || scheme == "synthetic";
}

// "X:" followed by a separator or the end: a Windows drive, not a scheme.
bool startsWithDrive(std::string_view text) noexcept {
    return text.size() >= 2 && isAlpha(text[0]) && text[1] == ':' &&
           (text.size() == 2 || text[2] == '\\' || text[2] == '/');
}

// "/X:" followed by a separator or the end: a drive behind a URI path slash.
bool startsWithSlashDrive(std::string_view path) noexcept {
    return path.size() >= 3 && path[0] == '/' && startsWithDrive(path.substr(1));
}

std::optional<std::string> schemeOf(std::string_view text) {
    const auto colon = text.find(':');
    if (colon == std::string_view::npos || colon < 2 || startsWithDrive(text)) {
        return std::nullopt;
    }
    const std::string_view candidate = text.substr(0, colon);
    if (!isAlpha(candidate[0])) {
        return std::nullopt;
    }
    std::string scheme;
    scheme.reserve(candidate.size());
    for (char c : candidate) {
        if (!isAlnum(c) && c != '+' && c != '-' && c != '.') {
            return std::nullopt;
        }
        scheme += toLower(c);
    }
    return scheme;
}

int hexValue(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

Result<std::vector<std::pair<std::string, std::string>>> parseQuery(std::string_view query) {
    std::vector<std::pair<std::string, std::string>> params;
    while (!query.empty()) {
        const auto amp = query.find('&');
        const std::string_view item = query.substr(0, amp);
        query = amp == std::string_view::npos ? std::string_view{} : query.substr(amp + 1);
        if (item.empty()) {
            continue;
        }
        const auto equals = item.find('=');
        auto name = percentDecode(item.substr(0, equals));
        if (!name) {
            return std::unexpected(std::move(name).error());
        }
        if (name->empty()) {
            return makeError(Errc::Format, "URI parameter has an empty name");
        }
        auto value = percentDecode(equals == std::string_view::npos ? std::string_view{}
                                                                    : item.substr(equals + 1));
        if (!value) {
            return std::unexpected(std::move(value).error());
        }
        params.emplace_back(std::move(*name), std::move(*value));
    }
    return params;
}

}  // namespace

std::optional<std::string_view> Uri::param(std::string_view name) const& {
    for (const auto& [key, value] : params) {
        if (key == name) {
            return value;
        }
    }
    return std::nullopt;
}

Result<std::string> percentDecode(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '%') {
            out += text[i];
            continue;
        }
        if (i + 2 >= text.size()) {
            return makeError(Errc::Format, "truncated percent escape in URI");
        }
        const int high = hexValue(text[i + 1]);
        const int low = hexValue(text[i + 2]);
        if (high < 0 || low < 0) {
            return makeError(Errc::Format, "invalid percent escape in URI");
        }
        const int byte = high * 16 + low;
        if (byte == 0) {
            return makeError(Errc::Format, "encoded NUL in URI");
        }
        out += static_cast<char>(byte);
        i += 2;
    }
    return out;
}

std::string percentEncode(std::string_view text, std::string_view keep) {
    static constexpr char HEX[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        const bool unreserved = isAlnum(c) || c == '-' || c == '.' || c == '_' || c == '~' ||
                                (c != '%' && keep.find(c) != std::string_view::npos);
        if (unreserved && byte < 0x80) {
            out += c;
        } else {
            out += '%';
            out += HEX[byte >> 4];
            out += HEX[byte & 0x0F];
        }
    }
    return out;
}

Result<Uri> parseUri(std::string_view text) {
    if (text.empty()) {
        return makeError(Errc::Format, "empty URI");
    }
    if (text.find('\0') != std::string_view::npos) {
        return makeError(Errc::Format, "NUL byte in URI");
    }
    Uri uri;
    const auto scheme = schemeOf(text);
    if (!scheme) {
        // Bare path: no percent-decoding (paths may contain '%'); '?' starts parameters.
        uri.scheme = "file";
        const auto question = text.find('?');
        uri.path = std::string(text.substr(0, question));
        if (uri.path.empty()) {
            return makeError(Errc::Format, "URI has an empty path");
        }
        if (question != std::string_view::npos) {
            auto params = parseQuery(text.substr(question + 1));
            if (!params) {
                return std::unexpected(std::move(params).error());
            }
            uri.params = std::move(*params);
        }
        return uri;
    }

    uri.scheme = *scheme;
    const std::string_view rest = text.substr(scheme->size() + 1);
    if (!isHierarchical(uri.scheme)) {
        if (rest.empty()) {
            return makeError(Errc::Format, "URI has an empty body");
        }
        uri.body = std::string(rest);
        return uri;
    }

    const auto question = rest.find('?');
    std::string_view path = rest.substr(0, question);
    if (path.starts_with("//")) {
        path.remove_prefix(2);
        const auto slash = path.find('/');
        const std::string_view authority = path.substr(0, slash);
        if (!authority.empty() && !equalsIgnoreCase(authority, "localhost")) {
            return makeError(Errc::Unsupported,
                             std::format("URI authority '{}' is not supported", authority));
        }
        path = slash == std::string_view::npos ? std::string_view{} : path.substr(slash);
    }
    auto decoded = percentDecode(path);
    if (!decoded) {
        return std::unexpected(std::move(decoded).error());
    }
    uri.path = std::move(*decoded);
    if (uri.path.empty()) {
        return makeError(Errc::Format, "URI has an empty path");
    }
    if (startsWithSlashDrive(uri.path)) {
        uri.path.erase(0, 1);  // file:///X:/media -> X:/media
    }
    if (question != std::string_view::npos) {
        auto params = parseQuery(rest.substr(question + 1));
        if (!params) {
            return std::unexpected(std::move(params).error());
        }
        uri.params = std::move(*params);
    }
    return uri;
}

std::string formatUri(const Uri& uri) {
    if (!isHierarchical(uri.scheme)) {
        return uri.scheme + ":" + uri.body;
    }
    // A bare path "/X:/..." would lose its '/' when read back as file:, so keep it bare. Bare
    // paths never contain '?', so the parameters can follow directly.
    const bool bare = uri.scheme == "file" && startsWithSlashDrive(uri.path);
    std::string text;
    if (bare) {
        text = uri.path;
    } else {
        text = uri.scheme + ":";
        if (uri.path.starts_with("//")) {
            text += "//";  // empty authority: the path must not become an authority
        }
        text += percentEncode(uri.path);
    }
    char separator = '?';
    for (const auto& [name, value] : uri.params) {
        text += separator;
        text += percentEncode(name, "");
        text += '=';
        text += percentEncode(value, "/:@\\");
        separator = '&';
    }
    return text;
}

}  // namespace dvv
