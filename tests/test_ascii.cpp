#include <doctest/doctest.h>

#include <string_view>

#include "dvv/base/ascii.h"

static_assert(dvv::isAsciiLetter('a') && dvv::isAsciiLetter('z') && dvv::isAsciiLetter('A') &&
              dvv::isAsciiLetter('Z'));
static_assert(!dvv::isAsciiLetter('@') && !dvv::isAsciiLetter('[') && !dvv::isAsciiLetter('`') &&
              !dvv::isAsciiLetter('{') && !dvv::isAsciiLetter('0') && !dvv::isAsciiLetter('\xC3'));
static_assert(dvv::asciiLower('A') == 'a' && dvv::asciiLower('Z') == 'z');
static_assert(dvv::asciiLower('@') == '@' && dvv::asciiLower('[') == '[' &&
              dvv::asciiLower('a') == 'a' && dvv::asciiLower('\xC4') == '\xC4');
static_assert(dvv::asciiUpper('a') == 'A' && dvv::asciiUpper('z') == 'Z');
static_assert(dvv::asciiUpper('`') == '`' && dvv::asciiUpper('{') == '{' &&
              dvv::asciiUpper('A') == 'A' && dvv::asciiUpper('\xE4') == '\xE4');
static_assert(dvv::equalsIgnoreAscii("RawReel", "rawreel"));
static_assert(!dvv::equalsIgnoreAscii("p@", "p`") && !dvv::equalsIgnoreAscii("ab", "abc"));
static_assert(!dvv::equalsIgnoreAscii("\xC3\x84", "\xC3\xA4"));  // no folding beyond ASCII

// Turkish locale: tolower('I') is not 'i'; single-byte locales change bytes >= 0x80.
static_assert(dvv::asciiLower('I') == 'i' && dvv::equalsIgnoreAscii("I", "i"));
static_assert(dvv::asciiUpper('i') == 'I' && dvv::asciiUpper('\xFD') == '\xFD');
static_assert(!dvv::equalsIgnoreAscii("\xFD", "i") && !dvv::equalsIgnoreAscii("\xDD", "I"));

namespace {

constexpr bool foldsOnlyAsciiLetters() {
    for (int value = 0; value < 256; ++value) {
        const char c = static_cast<char>(value);
        const bool upper = value >= 'A' && value <= 'Z';
        const bool lower = value >= 'a' && value <= 'z';
        if (dvv::asciiLower(c) != (upper ? static_cast<char>(value + ('a' - 'A')) : c)) {
            return false;
        }
        if (dvv::asciiUpper(c) != (lower ? static_cast<char>(value - ('a' - 'A')) : c)) {
            return false;
        }
        if (dvv::isAsciiLetter(c) != (upper || lower)) {
            return false;
        }
        const char folded = dvv::asciiLower(c);
        if (!dvv::equalsIgnoreAscii(std::string_view{&c, 1}, std::string_view{&folded, 1})) {
            return false;
        }
        if (value >= 0x80) {
            const char flipped = static_cast<char>(value ^ 0x20);
            if (dvv::equalsIgnoreAscii(std::string_view{&c, 1}, std::string_view{&flipped, 1})) {
                return false;
            }
        }
    }
    return true;
}

static_assert(foldsOnlyAsciiLetters());

}  // namespace

TEST_CASE("ascii helpers at run time") {
    CHECK(dvv::isAsciiLetter('a'));
    CHECK(dvv::isAsciiLetter('Z'));
    CHECK_FALSE(dvv::isAsciiLetter('@'));
    CHECK_FALSE(dvv::isAsciiLetter('['));
    CHECK_FALSE(dvv::isAsciiLetter('`'));
    CHECK_FALSE(dvv::isAsciiLetter('{'));
    CHECK_FALSE(dvv::isAsciiLetter('0'));
    CHECK_FALSE(dvv::isAsciiLetter('\xC3'));
    CHECK(dvv::asciiLower('A') == 'a');
    CHECK(dvv::asciiLower('Z') == 'z');
    CHECK(dvv::asciiLower('@') == '@');
    CHECK(dvv::asciiLower('[') == '[');
    CHECK(dvv::asciiLower('\xC4') == '\xC4');
    CHECK(dvv::asciiLower('I') == 'i');
    CHECK(dvv::asciiUpper('a') == 'A');
    CHECK(dvv::asciiUpper('z') == 'Z');
    CHECK(dvv::asciiUpper('`') == '`');
    CHECK(dvv::asciiUpper('{') == '{');
    CHECK(dvv::asciiUpper('\xE4') == '\xE4');
    CHECK(dvv::asciiUpper('i') == 'I');
    CHECK(dvv::asciiUpper('\xFD') == '\xFD');
    CHECK(dvv::equalsIgnoreAscii("RawReel", "rawreel"));
    CHECK(dvv::equalsIgnoreAscii("I", "i"));
    CHECK_FALSE(dvv::equalsIgnoreAscii("p@", "p`"));
    CHECK_FALSE(dvv::equalsIgnoreAscii("ab", "abc"));
    CHECK_FALSE(dvv::equalsIgnoreAscii("\xC3\x84", "\xC3\xA4"));
    CHECK_FALSE(dvv::equalsIgnoreAscii("\xFD", "i"));
    CHECK_FALSE(dvv::equalsIgnoreAscii("\xDD", "I"));
    CHECK(foldsOnlyAsciiLetters());
}
