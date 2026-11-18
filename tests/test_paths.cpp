#include <doctest/doctest.h>

#include <filesystem>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "dvv/base/environment.h"
#include "dvv/base/paths.h"
#include "dvv/base/utf8.h"
#include "paths_rules.h"

namespace {

using dvv::paths::detail::DirRule;
using dvv::paths::detail::DirSource;
using dvv::paths::detail::resolveDir;

// Sets variables for one test and restores them afterwards, also when a REQUIRE fails.
class ScopedEnvironment {
public:
    ScopedEnvironment() = default;
    ScopedEnvironment(const ScopedEnvironment&) = delete;
    ScopedEnvironment& operator=(const ScopedEnvironment&) = delete;
    ~ScopedEnvironment() {
        for (auto it = saved_.rbegin(); it != saved_.rend(); ++it) {
            dvv::setEnvironment(it->first, it->second.value_or(""));
        }
    }

    // An empty value removes the variable.
    void set(std::string_view name, std::string_view value) {
        saved_.emplace_back(std::string(name), dvv::getEnvironment(name));
        REQUIRE(dvv::setEnvironment(name, value));
    }

private:
    std::vector<std::pair<std::string, std::optional<std::string>>> saved_;
};

// The environment of the rule tests: a value may be empty, unlike a real variable on Windows.
using FakeEnvironment = std::vector<std::pair<std::string, std::string>>;
FakeEnvironment fake_environment;

std::optional<std::string> readFake(std::string_view name) {
    for (const auto& [key, value] : fake_environment) {
        if (key == name) {
            return value;
        }
    }
    return std::nullopt;
}

// Absolute on every OS; nothing is created there.
std::filesystem::path ruleRoot() {
    return std::filesystem::temp_directory_path() / "dvv-base-paths-rules";
}

// UTF-8 text of <ruleRoot>/<name>, as a variable's value.
std::string rootValue(std::string_view name) {
    return dvv::pathText(ruleRoot() / name);
}

// Paths are compared as UTF-8 text: on Windows doctest would stream a path through the ANSI code
// page and throw on a failing check whose path that code page cannot show.
#define CHECK_SAME_PATH(actual, expected) CHECK(dvv::pathText(actual) == dvv::pathText(expected))

// resolveDir of the rule {sources, tail} and `app` in the fake environment `environment`.
dvv::Result<std::filesystem::path> resolveIn(FakeEnvironment environment,
                                             std::initializer_list<DirSource> sources,
                                             std::string_view tail = "",
                                             std::string_view app = "app") {
    fake_environment = std::move(environment);
    const DirRule rule{std::span<const DirSource>(sources.begin(), sources.size()), tail};
    return resolveDir(rule, app, &readFake);
}

}  // namespace

TEST_CASE("paths rules: the first absolute variable wins; relative, app and tail follow") {
    const FakeEnvironment environment{{"DVV_A", rootValue("a")}, {"DVV_B", rootValue("b")}};
    CHECK_SAME_PATH(resolveIn(environment, {{"DVV_A", "sub/dir"}, {"DVV_B", ""}}, "cache").value(),
                    ruleRoot() / "a" / "sub/dir" / "app" / "cache");
    // Empty parts add no separator.
    const auto plain = resolveIn(environment, {{"DVV_A", ""}, {"DVV_B", ""}});
    REQUIRE(plain.has_value());
    CHECK_SAME_PATH(*plain, ruleRoot() / "a" / "app");
    CHECK_SAME_PATH(plain->filename(), "app");
    // The app name is UTF-8 (on Windows not the ANSI code page).
    CHECK_SAME_PATH(resolveIn(environment, {{"DVV_A", ""}}, "", "app-äö-データ").value(),
                    ruleRoot() / "a" / std::filesystem::path(u8"app-äö-データ"));
}

TEST_CASE("paths rules: unset, empty and relative values are skipped") {
    const auto expected = ruleRoot() / "b" / "app";
    const std::initializer_list<DirSource> sources{{"DVV_A", ""}, {"DVV_B", ""}};
    CHECK_SAME_PATH(resolveIn({{"DVV_B", rootValue("b")}}, sources).value(), expected);
    CHECK_SAME_PATH(resolveIn({{"DVV_A", ""}, {"DVV_B", rootValue("b")}}, sources).value(),
                    expected);
    CHECK_SAME_PATH(
        resolveIn({{"DVV_A", "relative/dir"}, {"DVV_B", rootValue("b")}}, sources).value(),
        expected);
}

TEST_CASE("paths rules: the error names every source tried") {
    const FakeEnvironment environment{{"DVV_B", "relative/dir"}};
    const std::initializer_list<DirSource> sources{{"DVV_A", ""}, {"DVV_B", ""}};
    const auto plain = resolveIn(environment, sources);
    REQUIRE_FALSE(plain.has_value());
    CHECK(plain.error().code == dvv::Errc::Io);
    CHECK(plain.error().message == "DVV_A is not set; DVV_B is not an absolute path");
}

TEST_CASE("paths rules: a rule without sources is an internal error") {
    const auto dir = resolveIn({}, {});
    REQUIRE_FALSE(dir.has_value());
    CHECK(dir.error().code == dvv::Errc::Internal);
}

TEST_CASE("paths rules: the real environment is read by default") {
    ScopedEnvironment environment;
    environment.set("DVV_BASE_TEST_PATHS_DIR", rootValue("env"));
    const std::initializer_list<DirSource> sources{{"DVV_BASE_TEST_PATHS_DIR", ""}};
    const DirRule rule{std::span<const DirSource>(sources.begin(), sources.size()), ""};
    CHECK_SAME_PATH(resolveDir(rule, "app").value(), ruleRoot() / "env" / "app");
}

TEST_CASE("user data and cache directories are absolute and end in the app name") {
    const auto data = dvv::paths::userDataDir("rawreel").value();
    CHECK(data.is_absolute());
    CHECK_SAME_PATH(data.filename(), "rawreel");
    const auto cache = dvv::paths::userCacheDir("rawreel").value();
    CHECK(cache.is_absolute());
    CHECK(dvv::pathText(cache).find("rawreel") != std::string::npos);
    CHECK(dvv::pathText(cache) != dvv::pathText(data));
}

TEST_CASE("app names must be plain directory names") {
    CHECK_FALSE(dvv::paths::userDataDir("").has_value());
    CHECK_FALSE(dvv::paths::userDataDir("a/b").has_value());
    CHECK_FALSE(dvv::paths::userCacheDir("a\\b").has_value());
    CHECK_FALSE(dvv::paths::userDataDir(".").has_value());
    CHECK_FALSE(dvv::paths::userDataDir("..").has_value());
    CHECK_FALSE(dvv::paths::userCacheDir(".").has_value());
    CHECK_FALSE(dvv::paths::userCacheDir("..").has_value());
}

TEST_CASE("app names must be valid UTF-8") {
    // A lone continuation byte: not valid UTF-8, so not an app name on any OS.
    const auto data = dvv::paths::userDataDir("app\x80");
    REQUIRE_FALSE(data.has_value());
    CHECK(data.error().code == dvv::Errc::Internal);
    CHECK(data.error().message == "application name must be valid UTF-8");
    const auto cache = dvv::paths::userCacheDir("app\xC3");
    REQUIRE_FALSE(cache.has_value());
    CHECK(cache.error().code == dvv::Errc::Internal);
}

TEST_CASE("app names must not contain NUL") {
    using namespace std::string_view_literals;
    const auto data = dvv::paths::userDataDir("app\0x"sv);
    REQUIRE_FALSE(data.has_value());
    CHECK(data.error().code == dvv::Errc::Internal);
    CHECK(data.error().message == "application name must be a plain directory name");
    CHECK_FALSE(dvv::paths::userCacheDir("\0"sv).has_value());
}

#if !defined(_WIN32) && !defined(__APPLE__)
TEST_CASE("XDG variables are honoured when absolute, else HOME") {
    ScopedEnvironment environment;
    environment.set("HOME", "/tmp/dvv-home");
    environment.set("XDG_DATA_HOME", "/tmp/dvv-xdg");
    environment.set("XDG_CACHE_HOME", "/tmp/dvv-xdg-cache");
    CHECK_SAME_PATH(dvv::paths::userDataDir("rawreel").value(), "/tmp/dvv-xdg/rawreel");
    CHECK_SAME_PATH(dvv::paths::userCacheDir("rawreel").value(), "/tmp/dvv-xdg-cache/rawreel");
    environment.set("XDG_DATA_HOME", "relative/dir");
    environment.set("XDG_CACHE_HOME", "relative/dir");
    CHECK_SAME_PATH(dvv::paths::userDataDir("rawreel").value(),
                    "/tmp/dvv-home/.local/share/rawreel");
    CHECK_SAME_PATH(dvv::paths::userCacheDir("rawreel").value(), "/tmp/dvv-home/.cache/rawreel");
    environment.set("XDG_DATA_HOME", "");
    environment.set("XDG_CACHE_HOME", "");
    CHECK_SAME_PATH(dvv::paths::userDataDir("rawreel").value(),
                    "/tmp/dvv-home/.local/share/rawreel");
    CHECK_SAME_PATH(dvv::paths::userCacheDir("rawreel").value(), "/tmp/dvv-home/.cache/rawreel");
}

TEST_CASE("Linux directories fail when neither XDG nor HOME is set") {
    ScopedEnvironment environment;
    environment.set("HOME", "");
    environment.set("XDG_DATA_HOME", "");
    environment.set("XDG_CACHE_HOME", "relative/dir");
    const auto data = dvv::paths::userDataDir("rawreel");
    REQUIRE_FALSE(data.has_value());
    CHECK(data.error().code == dvv::Errc::Io);
    CHECK(data.error().message == "XDG_DATA_HOME is not set; HOME is not set");
    const auto cache = dvv::paths::userCacheDir("rawreel");
    REQUIRE_FALSE(cache.has_value());
    CHECK(cache.error().code == dvv::Errc::Io);
    CHECK(cache.error().message == "XDG_CACHE_HOME is not an absolute path; HOME is not set");
}
#endif

#if defined(__APPLE__)
TEST_CASE("macOS directories live under ~/Library and ignore XDG variables") {
    const std::filesystem::path home = dvv::getEnvironment("HOME").value_or("");
    REQUIRE(home.is_absolute());
    ScopedEnvironment environment;
    environment.set("XDG_DATA_HOME", "/tmp/dvv-xdg");
    environment.set("XDG_CACHE_HOME", "/tmp/dvv-xdg");
    CHECK_SAME_PATH(dvv::paths::userDataDir("rawreel").value(),
                    home / "Library/Application Support/rawreel");
    CHECK_SAME_PATH(dvv::paths::userCacheDir("rawreel").value(), home / "Library/Caches/rawreel");
}

TEST_CASE("macOS directories follow HOME") {
    ScopedEnvironment environment;
    environment.set("HOME", "/tmp/dvv-home");
    CHECK_SAME_PATH(dvv::paths::userDataDir("rawreel").value(),
                    "/tmp/dvv-home/Library/Application Support/rawreel");
    CHECK_SAME_PATH(dvv::paths::userCacheDir("rawreel").value(),
                    "/tmp/dvv-home/Library/Caches/rawreel");
}

TEST_CASE("macOS directories fail when HOME is not set") {
    ScopedEnvironment environment;
    environment.set("HOME", "");
    const auto data = dvv::paths::userDataDir("rawreel");
    const auto cache = dvv::paths::userCacheDir("rawreel");
    REQUIRE_FALSE(data.has_value());
    CHECK(data.error().code == dvv::Errc::Io);
    CHECK(data.error().message == "HOME is not set");
    REQUIRE_FALSE(cache.has_value());
    CHECK(cache.error().code == dvv::Errc::Io);
}

TEST_CASE("macOS directories fail when HOME is relative") {
    ScopedEnvironment environment;
    environment.set("HOME", "relative/home");
    const auto data = dvv::paths::userDataDir("rawreel");
    const auto cache = dvv::paths::userCacheDir("rawreel");
    REQUIRE_FALSE(data.has_value());
    CHECK(data.error().code == dvv::Errc::Io);
    CHECK(data.error().message == "HOME is not an absolute path");
    REQUIRE_FALSE(cache.has_value());
    CHECK(cache.error().message == "HOME is not an absolute path");
}
#endif

#if defined(_WIN32)
TEST_CASE("Windows directories honour APPDATA and LOCALAPPDATA when absolute") {
    // Non-ASCII: the UTF-8 values must not go through the ANSI code page.
    const auto roaming = std::filesystem::temp_directory_path() / u8"dvv-base-äö-データ";
    const auto local = std::filesystem::temp_directory_path() / u8"dvv-base-local-äö-データ";
    const auto profile = std::filesystem::temp_directory_path() / u8"dvv-base-profile-äö-データ";
    ScopedEnvironment environment;
    environment.set("APPDATA", dvv::pathText(roaming));
    environment.set("LOCALAPPDATA", dvv::pathText(local));
    environment.set("USERPROFILE", dvv::pathText(profile));
    CHECK_SAME_PATH(dvv::paths::userDataDir("rawreel").value(), roaming / "rawreel");
    CHECK_SAME_PATH(dvv::paths::userCacheDir("rawreel").value(), local / "rawreel" / "cache");
}

TEST_CASE("Windows directories fall back to USERPROFILE\\AppData") {
    const auto profile = std::filesystem::temp_directory_path() / u8"dvv-base-profile-äö-データ";
    const auto data = profile / "AppData" / "Roaming" / "rawreel";
    const auto cache = profile / "AppData" / "Local" / "rawreel" / "cache";
    ScopedEnvironment environment;
    environment.set("USERPROFILE", dvv::pathText(profile));
    environment.set("APPDATA", "");
    environment.set("LOCALAPPDATA", "");
    CHECK_SAME_PATH(dvv::paths::userDataDir("rawreel").value(), data);
    CHECK_SAME_PATH(dvv::paths::userCacheDir("rawreel").value(), cache);
    environment.set("APPDATA", "relative\\dir");
    environment.set("LOCALAPPDATA", "relative\\dir");
    CHECK_SAME_PATH(dvv::paths::userDataDir("rawreel").value(), data);
    CHECK_SAME_PATH(dvv::paths::userCacheDir("rawreel").value(), cache);
}

TEST_CASE("Windows directories fail when APPDATA, LOCALAPPDATA and USERPROFILE are unset") {
    ScopedEnvironment environment;
    environment.set("APPDATA", "");
    environment.set("LOCALAPPDATA", "");
    environment.set("USERPROFILE", "");
    const auto data = dvv::paths::userDataDir("rawreel");
    REQUIRE_FALSE(data.has_value());
    CHECK(data.error().code == dvv::Errc::Io);
    CHECK(data.error().message == "APPDATA is not set; USERPROFILE is not set");
    const auto cache = dvv::paths::userCacheDir("rawreel");
    REQUIRE_FALSE(cache.has_value());
    CHECK(cache.error().code == dvv::Errc::Io);
    CHECK(cache.error().message == "LOCALAPPDATA is not set; USERPROFILE is not set");
}

TEST_CASE("Windows directories fail when APPDATA is unset and USERPROFILE is relative") {
    ScopedEnvironment environment;
    environment.set("APPDATA", "");
    environment.set("LOCALAPPDATA", "");
    environment.set("USERPROFILE", "relative\\profile");
    const auto data = dvv::paths::userDataDir("rawreel");
    REQUIRE_FALSE(data.has_value());
    CHECK(data.error().code == dvv::Errc::Io);
    CHECK(data.error().message == "APPDATA is not set; USERPROFILE is not an absolute path");
    const auto cache = dvv::paths::userCacheDir("rawreel");
    REQUIRE_FALSE(cache.has_value());
    CHECK(cache.error().code == dvv::Errc::Io);
    CHECK(cache.error().message == "LOCALAPPDATA is not set; USERPROFILE is not an absolute path");
}
#endif

TEST_CASE("executable path exists") {
    const auto path = dvv::paths::executablePath().value();
    CHECK(path.is_absolute());
    CHECK(std::filesystem::exists(path));
#if defined(__APPLE__)
    std::error_code error;
    CHECK_SAME_PATH(path, std::filesystem::canonical(path, error));
    CHECK(dvv::pathText(path.filename()).find("test") != std::string::npos);
#endif
}

TEST_CASE("ensureDirectory creates nested directories") {
    const auto dir = std::filesystem::temp_directory_path() / "dvv-base-paths-test" / "a" / "b";
    std::filesystem::remove_all(dir.parent_path().parent_path());
    REQUIRE(dvv::paths::ensureDirectory(dir).has_value());
    CHECK(std::filesystem::is_directory(dir));
    std::filesystem::remove_all(dir.parent_path().parent_path());
}
