#include <doctest/doctest.h>

#include <filesystem>

#include "dvv/base/environment.h"
#include "dvv/base/paths.h"

TEST_CASE("user data and cache directories are absolute and end in the app name") {
    const auto data = dvv::paths::userDataDir("rawreel").value();
    CHECK(data.is_absolute());
    CHECK(data.filename() == "rawreel");
    const auto cache = dvv::paths::userCacheDir("rawreel").value();
    CHECK(cache.is_absolute());
    CHECK(cache.string().find("rawreel") != std::string::npos);
    CHECK(cache != data);
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

#if !defined(_WIN32)
TEST_CASE("XDG variables are honoured when absolute") {
    const auto saved = dvv::getEnvironment("XDG_DATA_HOME");
    dvv::setEnvironment("XDG_DATA_HOME", "/tmp/dvv-xdg");
    CHECK(dvv::paths::userDataDir("rawreel").value() == "/tmp/dvv-xdg/rawreel");
    dvv::setEnvironment("XDG_DATA_HOME", "relative/dir");
    CHECK(dvv::paths::userDataDir("rawreel").value().is_absolute());
    dvv::setEnvironment("XDG_DATA_HOME", saved.value_or(""));
}
#endif

TEST_CASE("executable path exists") {
    const auto path = dvv::paths::executablePath().value();
    CHECK(path.is_absolute());
    CHECK(std::filesystem::exists(path));
}

TEST_CASE("ensureDirectory creates nested directories") {
    const auto dir = std::filesystem::temp_directory_path() / "dvv-base-paths-test" / "a" / "b";
    std::filesystem::remove_all(dir.parent_path().parent_path());
    REQUIRE(dvv::paths::ensureDirectory(dir).has_value());
    CHECK(std::filesystem::is_directory(dir));
    std::filesystem::remove_all(dir.parent_path().parent_path());
}
