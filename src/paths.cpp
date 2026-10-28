#include "dvv/base/paths.h"

#include <format>
#include <string>
#include <system_error>
#include <vector>

#include "dvv/base/environment.h"

#if defined(_WIN32)
// windows.h must come first.
// clang-format off
#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>
// clang-format on
#elif defined(__linux__)
#include <unistd.h>
#endif

namespace dvv::paths {

namespace {

Result<void> checkApp(std::string_view app) {
    if (app.empty() || app == "." || app == ".." ||
        app.find_first_of("/\\:") != std::string_view::npos) {
        return makeError(Errc::Internal, "application name must be a plain directory name");
    }
    return {};
}

#if defined(_WIN32)
Result<std::filesystem::path> knownFolder(const KNOWNFOLDERID& id) {
    PWSTR raw = nullptr;
    const HRESULT result = SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &raw);
    if (FAILED(result) || raw == nullptr) {
        CoTaskMemFree(raw);
        return makeError(Errc::Io, "cannot query a Windows known folder");
    }
    std::filesystem::path folder(raw);
    CoTaskMemFree(raw);
    return folder;
}
#else
// $variable when it holds an absolute path, otherwise $HOME/<fallback>.
Result<std::filesystem::path> xdgDir(std::string_view variable, std::string_view fallback) {
    if (const auto value = getEnvironment(variable)) {
        std::filesystem::path dir(*value);
        if (dir.is_absolute()) {
            return dir;
        }
    }
    const auto home = getEnvironment("HOME");
    if (!home || home->empty()) {
        return makeError(Errc::Io, "HOME is not set");
    }
    return std::filesystem::path(*home) / fallback;
}
#endif

}  // namespace

Result<std::filesystem::path> userDataDir(std::string_view app) {
    DVV_TRY_VOID(checkApp(app));
#if defined(_WIN32)
    DVV_TRY(auto base, knownFolder(FOLDERID_RoamingAppData));
#else
    DVV_TRY(auto base, xdgDir("XDG_DATA_HOME", ".local/share"));
#endif
    return base / app;
}

Result<std::filesystem::path> userCacheDir(std::string_view app) {
    DVV_TRY_VOID(checkApp(app));
#if defined(_WIN32)
    DVV_TRY(auto base, knownFolder(FOLDERID_LocalAppData));
    return base / app / "cache";
#else
    DVV_TRY(auto base, xdgDir("XDG_CACHE_HOME", ".cache"));
    return base / app;
#endif
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
