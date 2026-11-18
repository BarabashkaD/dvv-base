#include "dvv/base/environment.h"

#include <cstdlib>

#include "dvv/base/utf8.h"

#if defined(_WIN32)
#include <windows.h>

#include "dvv/base/utf8_win32.h"
#endif

namespace dvv {

namespace {

bool validValue(std::string_view value) noexcept {
    return value.find('\0') == std::string_view::npos && isValidUtf8(value);
}

bool validName(std::string_view name) noexcept {
    return !name.empty() && name.find('=') == std::string_view::npos && validValue(name);
}

}  // namespace

std::optional<std::string> getEnvironment(std::string_view name) {
    if (!validName(name)) {
        return std::nullopt;
    }
#if defined(_WIN32)
    const std::wstring wide_name = wideFromUtf8(name);
    std::wstring buffer(128, L'\0');
    for (;;) {
        SetLastError(ERROR_SUCCESS);
        const DWORD length = GetEnvironmentVariableW(wide_name.c_str(), buffer.data(),
                                                     static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            if (GetLastError() == ERROR_ENVVAR_NOT_FOUND) {
                return std::nullopt;
            }
            return std::string();  // set to an empty value
        }
        if (length < buffer.size()) {
            buffer.resize(length);
            return utf8FromWide(buffer);
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
    const std::wstring wide_name = wideFromUtf8(name);
    const std::wstring wide_value = wideFromUtf8(value);
    // The UCRT fast-fails (invalid-parameter handler) at _MAX_ENV; the OS has the same limit.
    if (wide_name.size() >= _MAX_ENV || wide_value.size() >= _MAX_ENV) {
        return false;
    }
    if (_wputenv_s(wide_name.c_str(), wide_value.c_str()) != 0) {
        return false;
    }
    // _wputenv_s skips the OS call when the CRT copy lacks the name (a variable set by
    // SetEnvironmentVariableW, or inherited with a name outside the ANSI code page).
    if (wide_value.empty() && SetEnvironmentVariableW(wide_name.c_str(), nullptr) == 0) {
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
