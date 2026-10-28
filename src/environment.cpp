#include "dvv/base/environment.h"

#include <cstddef>
#include <cstdlib>

#if defined(_WIN32)
#include <climits>

#include <windows.h>
#endif

namespace dvv {

namespace {

// Well-formed UTF-8 (RFC 3629): no overlong forms, no surrogates, nothing above U+10FFFF.
bool isValidUtf8(std::string_view text) noexcept {
    std::size_t i = 0;
    while (i < text.size()) {
        const auto lead = static_cast<unsigned char>(text[i]);
        if (lead < 0x80) {
            ++i;
            continue;
        }
        std::size_t length = 0;
        char32_t code = 0;
        char32_t minimum = 0;
        if ((lead & 0xE0) == 0xC0) {
            length = 2;
            code = lead & 0x1F;
            minimum = 0x80;
        } else if ((lead & 0xF0) == 0xE0) {
            length = 3;
            code = lead & 0x0F;
            minimum = 0x800;
        } else if ((lead & 0xF8) == 0xF0) {
            length = 4;
            code = lead & 0x07;
            minimum = 0x10000;
        } else {
            return false;
        }
        if (text.size() - i < length) {
            return false;
        }
        for (std::size_t k = 1; k < length; ++k) {
            const auto next = static_cast<unsigned char>(text[i + k]);
            if ((next & 0xC0) != 0x80) {
                return false;
            }
            code = (code << 6) | (next & 0x3F);
        }
        if (code < minimum || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) {
            return false;
        }
        i += length;
    }
    return true;
}

bool validValue(std::string_view value) noexcept {
    return value.find('\0') == std::string_view::npos && isValidUtf8(value);
}

bool validName(std::string_view name) noexcept {
    return !name.empty() && name.find('=') == std::string_view::npos && validValue(name);
}

#if defined(_WIN32)
std::optional<std::wstring> toWide(std::string_view text) {
    if (text.empty()) {
        return std::wstring();
    }
    if (text.size() > static_cast<std::size_t>(INT_MAX)) {
        return std::nullopt;
    }
    const int length = static_cast<int>(text.size());
    const int size =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length, nullptr, 0);
    if (size <= 0) {
        return std::nullopt;
    }
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length, wide.data(),
                            size) != size) {
        return std::nullopt;
    }
    return wide;
}

std::optional<std::string> toUtf8(std::wstring_view text) {
    if (text.empty()) {
        return std::string();
    }
    if (text.size() > static_cast<std::size_t>(INT_MAX)) {
        return std::nullopt;
    }
    const int length = static_cast<int>(text.size());
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), length,
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return std::nullopt;
    }
    std::string narrow(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), length, narrow.data(), size,
                            nullptr, nullptr) != size) {
        return std::nullopt;
    }
    return narrow;
}
#endif

}  // namespace

std::optional<std::string> getEnvironment(std::string_view name) {
    if (!validName(name)) {
        return std::nullopt;
    }
#if defined(_WIN32)
    const auto wide_name = toWide(name);
    if (!wide_name) {
        return std::nullopt;
    }
    std::wstring buffer(128, L'\0');
    for (;;) {
        SetLastError(ERROR_SUCCESS);
        const DWORD length = GetEnvironmentVariableW(wide_name->c_str(), buffer.data(),
                                                     static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            if (GetLastError() == ERROR_ENVVAR_NOT_FOUND) {
                return std::nullopt;
            }
            return std::string();  // set to an empty value
        }
        if (length < buffer.size()) {
            buffer.resize(length);
            return toUtf8(buffer);
        }
        buffer.resize(length);  // too small: `length` includes the terminator; try again
    }
#else
    const std::string key(name);
    const char* value = std::getenv(key.c_str());
    if (value == nullptr) {
        return std::nullopt;
    }
    return std::string(value);
#endif
}

bool setEnvironment(std::string_view name, std::string_view value) {
    if (!validName(name) || !validValue(value)) {
        return false;
    }
#if defined(_WIN32)
    const auto wide_name = toWide(name);
    const auto wide_value = toWide(value);
    if (!wide_name || !wide_value) {
        return false;
    }
    // The UCRT fast-fails (invalid-parameter handler) at _MAX_ENV; the OS has the same limit.
    if (wide_name->size() >= _MAX_ENV || wide_value->size() >= _MAX_ENV) {
        return false;
    }
    if (_wputenv_s(wide_name->c_str(), wide_value->c_str()) != 0) {
        return false;
    }
    // _wputenv_s skips the OS call when the CRT copy lacks the name (a variable set by
    // SetEnvironmentVariableW, or inherited with a name outside the ANSI code page).
    if (wide_value->empty() && SetEnvironmentVariableW(wide_name->c_str(), nullptr) == 0) {
        return GetLastError() == ERROR_ENVVAR_NOT_FOUND;
    }
    return true;
#else
    const std::string key(name);
    if (value.empty()) {
        return unsetenv(key.c_str()) == 0;
    }
    const std::string text(value);
    return setenv(key.c_str(), text.c_str(), 1) == 0;
#endif
}

}  // namespace dvv
