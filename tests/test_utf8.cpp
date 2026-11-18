#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "dvv/base/uri.h"
#include "dvv/base/utf8.h"
#include "utf8_detail.h"

namespace {

const std::vector<std::string>& wellFormed() {
    static const std::vector<std::string> LIST{"", "abc", "a\xC3\xA4\xE6\x97\xA5\xF0\x9F\x8E\x9E"};
    return LIST;
}

const std::vector<std::string>& illFormed() {
    static const std::vector<std::string> LIST{"\xC0\xAF",         "\xE0\x80\x80", "\xED\xA0\x80",
                                               "\xF4\x90\x80\x80", "\x80",         "\xE6\x97",
                                               "\xF5\x80\x80\x80"};
    return LIST;
}

constexpr std::string_view FFFD = "\xEF\xBF\xBD";

std::string repeat(int count) {
    std::string out;
    for (int i = 0; i < count; ++i) {
        out += FFFD;
    }
    return out;
}

#if defined(__linux__)
// Unique temporary directory per instance, removed on destruction even when a check fails.
class TempDir {
public:
    explicit TempDir(const std::string& prefix) {
        std::random_device random;
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                std::format("{}-{}-{:08x}", prefix, stamp, random());
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};
#endif

}  // namespace

TEST_CASE("isValidUtf8 accepts well-formed text") {
    for (const auto& text : wellFormed()) {
        CHECK(dvv::isValidUtf8(text));
    }
}

TEST_CASE("isValidUtf8 rejects ill-formed text") {
    for (const auto& text : illFormed()) {
        CHECK_FALSE(dvv::isValidUtf8(text));
    }
}

TEST_CASE("display text replaces each maximal subpart with one U+FFFD") {
    using dvv::detail::replaceIllFormed;
    CHECK(replaceIllFormed("a\xFF"
                           "b") == "a" + std::string{FFFD} + "b");
    CHECK(replaceIllFormed("\xE6\x97") == repeat(1));
    CHECK(replaceIllFormed("\xF0\x9F\x8E"
                           "a") == repeat(1) + "a");
    CHECK(replaceIllFormed("\xC0\xAF") == repeat(2));
    CHECK(replaceIllFormed("\xE0\x80\x80") == repeat(3));
    CHECK(replaceIllFormed("\xED\xA0\x80") == repeat(3));
    CHECK(replaceIllFormed("\xF4\x90\x80\x80") == repeat(4));
    CHECK(replaceIllFormed("\xF5\x80\x80\x80") == repeat(4));
}

TEST_CASE("the validator and the replacement agree") {
    for (const auto* list : {&wellFormed(), &illFormed()}) {
        for (const auto& text : *list) {
            CHECK(dvv::isValidUtf8(text) == (dvv::detail::replaceIllFormed(text) == text));
        }
    }
}

TEST_CASE("path text round-trips valid text") {
    for (const std::string text :
         {"a\xC3\xA4\xE6\x97\xA5\xF0\x9F\x8E\x9E", "\xC3\xBC/\xE6\x97\xA5.txt"}) {
        const std::filesystem::path p = dvv::pathFromText(text);
        CHECK(dvv::pathText(p) == text);
        CHECK(dvv::pathDisplayText(p) == text);
        CHECK(dvv::pathFromText(dvv::pathText(p)).native() == p.native());
    }
}

TEST_CASE("utf16 decoder replaces or rejects unpaired surrogates") {
    using dvv::detail::BadUnits;
    using dvv::detail::utf8FromUtf16;
    const std::u16string pair = u"\U0001F39E";
    const std::optional<std::string> pair_bytes{"\xF0\x9F\x8E\x9E"};
    CHECK(utf8FromUtf16<char16_t>(std::u16string_view{pair}, BadUnits::Replace) == pair_bytes);
    CHECK(utf8FromUtf16<char16_t>(std::u16string_view{pair}, BadUnits::Reject) == pair_bytes);

    const std::u16string high_at_end{u'a', char16_t{0xD800}};
    const std::u16string low_alone{char16_t{0xDC00}};
    const std::u16string high_then_plain{char16_t{0xD800}, u'a'};
    CHECK(utf8FromUtf16<char16_t>(std::u16string_view{high_at_end}, BadUnits::Replace) ==
          std::optional<std::string>{"a" + std::string{FFFD}});
    CHECK(utf8FromUtf16<char16_t>(std::u16string_view{low_alone}, BadUnits::Replace) ==
          std::optional<std::string>{std::string{FFFD}});
    CHECK(utf8FromUtf16<char16_t>(std::u16string_view{high_then_plain}, BadUnits::Replace) ==
          std::optional<std::string>{std::string{FFFD} + "a"});
    CHECK(utf8FromUtf16<char16_t>(std::u16string_view{high_at_end}, BadUnits::Reject) ==
          std::nullopt);
    CHECK(utf8FromUtf16<char16_t>(std::u16string_view{low_alone}, BadUnits::Reject) ==
          std::nullopt);
    CHECK(utf8FromUtf16<char16_t>(std::u16string_view{high_then_plain}, BadUnits::Reject) ==
          std::nullopt);

    const std::u16string empty;
    CHECK(utf8FromUtf16<char16_t>(std::u16string_view{empty}, BadUnits::Reject) ==
          std::optional<std::string>{""});
}

TEST_CASE("utf16 encoder replaces maximal subparts") {
    using dvv::detail::utf16FromUtf8;
    CHECK(utf16FromUtf8<char16_t>("a\xC3\xA4\xF0\x9F\x8E\x9E") == u"aä\U0001F39E");
    CHECK(utf16FromUtf8<char16_t>("a\xE6\x97") == std::u16string{u'a', char16_t{0xFFFD}});
    CHECK(utf16FromUtf8<char16_t>("") == u"");
}

#if !defined(_WIN32)
TEST_CASE("path text keeps the bytes of a non-UTF-8 name") {
    const std::filesystem::path p{
        std::string("a\xFF"
                    "b")};
    CHECK(dvv::pathText(p) == std::string("a\xFF"
                                          "b"));
    CHECK(dvv::pathFromText(dvv::pathText(p)).native() == p.native());
    CHECK(dvv::pathDisplayText(p) == "a" + std::string{FFFD} + "b");
}
#endif

#if defined(__linux__)
TEST_CASE("a non-UTF-8 file name opens through its path text") {
    const TempDir dir("dvv-utf8");
    const std::filesystem::path p = dir.path() / std::string(
                                                     "x\xFF"
                                                     ".bin");
    {
        std::ofstream file(p);
        file << "x";
    }
    const auto decoded = dvv::percentDecode(dvv::percentEncode(dvv::pathText(p)));
    CHECK(decoded.has_value());
    if (decoded) {
        CHECK(std::filesystem::exists(dvv::pathFromText(*decoded)));
    }
}
#endif
