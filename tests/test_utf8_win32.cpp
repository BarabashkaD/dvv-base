#include <doctest/doctest.h>

#include <filesystem>
#include <string>

#include "dvv/base/utf8.h"
#include "dvv/base/utf8_win32.h"

TEST_CASE("utf8FromWide converts") {
    CHECK(dvv::utf8FromWide(L"") == "");
    CHECK(dvv::utf8FromWide(L"aä日") == "a\xC3\xA4\xE6\x97\xA5");
    const std::wstring pair{static_cast<wchar_t>(0xD83C), static_cast<wchar_t>(0xDF9E)};
    CHECK(dvv::utf8FromWide(pair) == "\xF0\x9F\x8E\x9E");
}

TEST_CASE("utf8FromWide rejects unpaired surrogates") {
    CHECK_FALSE(dvv::utf8FromWide(std::wstring{static_cast<wchar_t>(0xD800)}).has_value());
    CHECK_FALSE(dvv::utf8FromWide(std::wstring{L'a', static_cast<wchar_t>(0xDC00)}).has_value());
}

TEST_CASE("wideFromUtf8 converts and replaces") {
    CHECK(dvv::wideFromUtf8("a\xC3\xA4") == L"aä");
    CHECK(dvv::wideFromUtf8("a\xFF") == std::wstring{L'a', static_cast<wchar_t>(0xFFFD)});
    CHECK(dvv::wideFromUtf8("a\xE6\x97") == std::wstring{L'a', static_cast<wchar_t>(0xFFFD)});
}

TEST_CASE("a name with no UTF-8 form") {
    const std::filesystem::path p(std::wstring{L'a', static_cast<wchar_t>(0xD800), L'b'});
    const std::string expected =
        "a\xEF\xBF\xBD"
        "b";
    CHECK(dvv::pathText(p) == expected);
    CHECK(dvv::pathDisplayText(p) == expected);
    CHECK(dvv::pathFromText("a\xFF"
                            "b")
              .native() == std::wstring{L'a', static_cast<wchar_t>(0xFFFD), L'b'});
}
