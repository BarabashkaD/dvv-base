#include <doctest/doctest.h>

#include <cstddef>
#include <iterator>
#include <string>
#include <string_view>

#include "dvv/base/environment.h"

#if defined(_WIN32)
#include <cstdlib>

#include <windows.h>
#endif

namespace {

// "Grüße-日本-🎞" (U+00FC, U+00DF, U+65E5, U+672C, U+1F39E)
constexpr std::string_view UTF8_SAMPLE =
    "Gr\xC3\xBC\xC3\x9F"
    "e-\xE6\x97\xA5\xE6\x9C\xAC-\xF0\x9F\x8E\x9E";
#if defined(_WIN32)
constexpr std::wstring_view WIDE_SAMPLE = L"Grüße-日本-\U0001F39E";
#endif

}  // namespace

TEST_CASE("environment round trip") {
    REQUIRE(dvv::setEnvironment("DVV_BASE_TEST_VARIABLE", "value"));
    CHECK(dvv::getEnvironment("DVV_BASE_TEST_VARIABLE") == "value");
    REQUIRE(dvv::setEnvironment("DVV_BASE_TEST_VARIABLE", ""));
    CHECK_FALSE(dvv::getEnvironment("DVV_BASE_TEST_VARIABLE").has_value());
}

TEST_CASE("environment values round-trip as UTF-8") {
    REQUIRE(dvv::setEnvironment("DVV_BASE_TEST_UTF8", UTF8_SAMPLE));
    CHECK(dvv::getEnvironment("DVV_BASE_TEST_UTF8") == std::string(UTF8_SAMPLE));
#if defined(_WIN32)
    {
        std::wstring buffer(64, L'\0');
        const DWORD length = GetEnvironmentVariableW(L"DVV_BASE_TEST_UTF8", buffer.data(),
                                                     static_cast<DWORD>(buffer.size()));
        REQUIRE(length == WIDE_SAMPLE.size());
        buffer.resize(length);
        CHECK(buffer == WIDE_SAMPLE);
    }
#endif
    REQUIRE(dvv::setEnvironment("DVV_BASE_TEST_UTF8", ""));
    CHECK_FALSE(dvv::getEnvironment("DVV_BASE_TEST_UTF8").has_value());
#if defined(_WIN32)
    {
        std::wstring buffer(64, L'\0');
        SetLastError(ERROR_SUCCESS);
        const DWORD length = GetEnvironmentVariableW(L"DVV_BASE_TEST_UTF8", buffer.data(),
                                                     static_cast<DWORD>(buffer.size()));
        CHECK(length == 0);
        CHECK(GetLastError() == ERROR_ENVVAR_NOT_FOUND);
    }
#endif
}

TEST_CASE("non-ASCII names work") {
    const std::string name = "DVV_BASE_TEST_\xC3\x9C";
    REQUIRE(dvv::setEnvironment(name, "x"));
    CHECK(dvv::getEnvironment(name) == "x");
    REQUIRE(dvv::setEnvironment(name, ""));
    CHECK_FALSE(dvv::getEnvironment(name).has_value());
}

TEST_CASE("invalid names are rejected") {
    const std::string names[] = {
        "",
        "A=B",
        "=A",
        "\xC3",
        "\xC0\xAF",
        "\xED\xA0\x80",
        std::string("DVV_BASE_TEST_NUL\0X", 19),
    };
    for (const auto& name : names) {
        CAPTURE(name.size());
        CHECK_FALSE(dvv::setEnvironment(name, "v"));
        CHECK_FALSE(dvv::getEnvironment(name).has_value());
    }
    CHECK_FALSE(dvv::getEnvironment("DVV_BASE_TEST_NUL").has_value());
}

TEST_CASE("invalid values are rejected and change nothing") {
    REQUIRE(dvv::setEnvironment("DVV_BASE_TEST_BAD", "kept"));
    const std::string values[] = {
        "\xFF",
        "\xF4\x90\x80\x80",
        std::string("a\0b", 3),
        "\x80",                  // stray continuation byte
        "\xC3\x28",              // bad continuation byte
        "\xE6\x97",              // truncated 3-byte sequence
        "\xE0\x80\xAF",          // 3-byte overlong
        "\xF0\x80\x80\xAF",      // 4-byte overlong
        "\xF5\x80\x80\x80",      // lead byte F5
        "\xF8\x88\x80\x80\x80",  // lead byte F8
    };
    for (std::size_t index = 0; index < std::size(values); ++index) {
        CAPTURE(index);
        CHECK_FALSE(dvv::setEnvironment("DVV_BASE_TEST_BAD", values[index]));
    }
    CHECK(dvv::getEnvironment("DVV_BASE_TEST_BAD") == "kept");
    dvv::setEnvironment("DVV_BASE_TEST_BAD", "");
}

TEST_CASE("boundary code points are accepted") {
    // U+007F, U+0080, U+07FF, U+0800, U+D7FF, U+E000, U+FFFF, U+10000, U+10FFFF
    const std::string values[] = {"\x7F",         "\xC2\x80",         "\xDF\xBF",
                                  "\xE0\xA0\x80", "\xED\x9F\xBF",     "\xEE\x80\x80",
                                  "\xEF\xBF\xBF", "\xF0\x90\x80\x80", "\xF4\x8F\xBF\xBF"};
    for (std::size_t index = 0; index < std::size(values); ++index) {
        CAPTURE(index);
        REQUIRE(dvv::setEnvironment("DVV_BASE_TEST_EDGE", values[index]));
        CHECK(dvv::getEnvironment("DVV_BASE_TEST_EDGE") == values[index]);
    }
    REQUIRE(dvv::setEnvironment("DVV_BASE_TEST_EDGE", ""));
}

TEST_CASE("a variable inherited from the parent process reads as UTF-8") {
    const auto value = dvv::getEnvironment("DVV_BASE_TEST_INHERITED");
    if (!value) {
        MESSAGE("DVV_BASE_TEST_INHERITED is set only by CTest; skipped");
        return;
    }
    CHECK(*value == std::string(UTF8_SAMPLE));
}

#if defined(_WIN32)
TEST_CASE("a value that is not valid UTF-16 reads as nullopt") {
    // The literal is split so that `b` is not read as a hex digit.
    REQUIRE(_wputenv_s(L"DVV_BASE_TEST_SURROGATE",
                       L"a\xD800"
                       L"b") == 0);
    CHECK_FALSE(dvv::getEnvironment("DVV_BASE_TEST_SURROGATE").has_value());
    _wputenv_s(L"DVV_BASE_TEST_SURROGATE", L"");
}

TEST_CASE("an over-long value is rejected, not fatal") {
    CHECK_FALSE(dvv::setEnvironment("DVV_BASE_TEST_LONG", std::string(40000, 'a')));
    CHECK_FALSE(dvv::getEnvironment("DVV_BASE_TEST_LONG").has_value());
}

TEST_CASE("removal also clears a variable known only to the OS") {
    REQUIRE(dvv::setEnvironment("DVV_BASE_TEST_WARM", "1"));  // creates the CRT wide copy
    REQUIRE(SetEnvironmentVariableW(L"DVV_BASE_TEST_OS_ONLY", L"x") != 0);
    REQUIRE(dvv::setEnvironment("DVV_BASE_TEST_OS_ONLY", ""));
    CHECK_FALSE(dvv::getEnvironment("DVV_BASE_TEST_OS_ONLY").has_value());
    dvv::setEnvironment("DVV_BASE_TEST_WARM", "");
}
#endif
