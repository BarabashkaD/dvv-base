#include "dvv/base/paths.h"

#include <climits>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "paths_rules.h"
#include "utf8_path.h"

#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <unistd.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace dvv::paths {

namespace detail {

namespace {

using dvv::detail::pathFromUtf8;

// <base>/<relative>/<app>/<tail>; an empty part adds nothing (no trailing separator). Every part
// goes through pathFromUtf8, so the app name (UTF-8) and the ASCII literals are joined the same
// way.
std::filesystem::path join(std::filesystem::path base, std::string_view relative,
                           std::string_view app, std::string_view tail) {
    for (const std::string_view part : {relative, app, tail}) {
        if (!part.empty()) {
            base /= pathFromUtf8(part);
        }
    }
    return base;
}

}  // namespace

Result<std::filesystem::path> resolveDir(const DirRule& rule, std::string_view app,
                                         EnvironmentReader read) {
    if (rule.sources.empty()) {
        return makeError(Errc::Internal, "a folder rule has no source");
    }
    std::string tried;
    // Adds one source's reason to the list.
    const auto note = [&tried](std::string_view reason) {
        std::format_to(std::back_inserter(tried), "{}{}", tried.empty() ? "" : "; ", reason);
    };
    for (const DirSource& source : rule.sources) {
        const std::optional<std::string> value = read(source.variable);
        if (!value || value->empty()) {
            note(std::format("{} is not set", source.variable));
            continue;
        }
        auto base = pathFromUtf8(*value);
        if (!base.is_absolute()) {
            note(std::format("{} is not an absolute path", source.variable));
            continue;
        }
        return join(std::move(base), source.relative, app, rule.tail);
    }
    return makeError(Errc::Io, std::move(tried));
}

}  // namespace detail

namespace {

Result<void> checkApp(std::string_view app) {
    // Not allowed in the name; the explicit length keeps the NUL in the set.
    constexpr std::string_view FORBIDDEN_APP_CHARS{"/\\:\0", 4};
    if (app.empty() || app == "." || app == ".." ||
        app.find_first_of(FORBIDDEN_APP_CHARS) != std::string_view::npos) {
        return makeError(Errc::Internal, "application name must be a plain directory name");
    }
    // pathFromUtf8 (path from u8string) would throw on Windows for a name that is not valid UTF-8.
    if (!dvv::detail::isValidUtf8(app)) {
        return makeError(Errc::Internal, "application name must be valid UTF-8");
    }
    return {};
}

}  // namespace

Result<std::filesystem::path> userDataDir(std::string_view app) {
    DVV_TRY_VOID(checkApp(app));
    return detail::resolveDir(detail::userDataRule(), app);
}

Result<std::filesystem::path> userCacheDir(std::string_view app) {
    DVV_TRY_VOID(checkApp(app));
    return detail::resolveDir(detail::userCacheRule(), app);
}

Result<std::filesystem::path> executablePath() {
#if defined(_WIN32)
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const DWORD length =
            GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return makeError(Errc::Io, "GetModuleFileNameW failed");
        }
        if (length < buffer.size()) {
            return std::filesystem::path(std::wstring(buffer.data(), length));
        }
        buffer.resize(buffer.size() * 2);
    }
#elif defined(__linux__)
    std::error_code error;
    auto path = std::filesystem::read_symlink("/proc/self/exe", error);
    if (error) {
        return makeError(Errc::Io, std::format("cannot read /proc/self/exe: {}", error.message()));
    }
    return path;
#elif defined(__APPLE__)
    std::vector<char> buffer(PATH_MAX);
    auto size = static_cast<uint32_t>(buffer.size());
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        // Too small: `size` now holds the required length.
        buffer.resize(size);
        if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
            return makeError(Errc::Io, "_NSGetExecutablePath failed");
        }
    }
    std::error_code error;
    auto path = std::filesystem::canonical(buffer.data(), error);
    if (error) {
        return makeError(Errc::Io,
                         std::format("cannot resolve the executable path: {}", error.message()));
    }
    return path;
#else
    return makeError(Errc::Unsupported, "executablePath is not implemented on this platform");
#endif
}

Result<void> ensureDirectory(const std::filesystem::path& directory) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        return makeError(Errc::Io, std::format("cannot create directory: {}", error.message()));
    }
    return {};
}

}  // namespace dvv::paths
