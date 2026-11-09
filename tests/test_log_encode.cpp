#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include "dvv/base/log_encode.h"
#include "log_test_support.h"

namespace {

using dvv_test::Colour;

// Encodes the arguments, decodes them with the same types and formats them.
template <class... Args>
std::string roundTrip(std::format_string<Args...> fmt, const Args&... args) {
    constexpr std::size_t BUDGET = 1U << 16U;
    std::vector<std::byte> buffer(dvv::detail::encodedSize(BUDGET, args...));
    std::byte* end = dvv::detail::encodeArguments(buffer.data(), BUDGET, args...);
    CHECK(end == buffer.data() + buffer.size());
    std::string out;
    dvv::detail::decodeAndFormat<Args...>(buffer.data(), fmt.get(), out);
    return out;
}

// Encodes one string with the given budget and returns what the log thread would print.
std::string encodeDecodeWithBudget(const std::string& text, std::size_t budget) {
    std::vector<std::byte> buffer(dvv::detail::encodedSize(budget, text));
    CHECK(buffer.size() <= budget);
    dvv::detail::encodeArguments(buffer.data(), budget, text);
    std::string out;
    dvv::detail::decodeAndFormat<std::string>(buffer.data(), "{}", out);
    return out;
}

}  // namespace

TEST_CASE("encode: every deferrable type prints as std::format does") {
    CHECK(roundTrip("{} {} {}", true, 'x', static_cast<signed char>(-3)) == "true x -3");
    CHECK(roundTrip("{:>8}|{:x}|{:.3f}", std::string("ab"), 255U, 2.0) == "      ab|ff|2.000");
    CHECK(roundTrip("{} {}", std::int64_t{-1}, std::uint64_t{18446744073709551615ULL}) ==
          "-1 18446744073709551615");
    const char array[] = "arr";
    char buffer[8] = "ab";
    CHECK(roundTrip("{} {} {}", array, buffer, std::string_view("sv")) == "arr ab sv");
    const char* null_text = nullptr;
    CHECK(roundTrip("{}", null_text) == "(null)");
    int value = 0;
    const void* address = &value;
    CHECK(roundTrip("{}", address) == std::format("{}", address));
    CHECK(roundTrip("{} {}", 1.5F, 2.25L) == "1.5 2.25");
}

TEST_CASE("encode: enums, classes and paths are not deferrable") {
    CHECK_FALSE(dvv::detail::LOG_DEFERRABLE<Colour>);
    CHECK_FALSE(dvv::detail::LOG_DEFERRABLE<std::filesystem::path>);
    CHECK_FALSE(dvv::detail::LOG_DEFERRABLE<wchar_t>);
    CHECK_FALSE(dvv::detail::LOG_DEFERRABLE<char8_t>);
    CHECK(dvv::detail::LOG_DEFERRABLE<char[4]>);
    CHECK(dvv::detail::LOG_DEFERRABLE<const std::string&>);
    CHECK(dvv::detail::LOG_DEFERRABLE<std::nullptr_t>);
    CHECK(dvv::detail::LOG_ALL_DEFERRABLE<int, const char*, std::string_view>);
    CHECK_FALSE(dvv::detail::LOG_ALL_DEFERRABLE<int, Colour>);
}

TEST_CASE("encode: a string over the budget is cut on a code point, marker inside the budget") {
    const std::string text = std::string(100, 'a') + "日本語日本";  // 3-byte code points
    // budget = tag + length + 101 text bytes + the 8-byte marker: the cut must not split 日
    CHECK(encodeDecodeWithBudget(text, 1 + 4 + 101 + 8) == std::string(100, 'a') + "…[cut]");
}

TEST_CASE("encode: the longest strings are cut so that all arguments fit") {
    const std::string longer(200, 'b');
    const std::string shorter(10, 'c');
    // budget 70 = int (5) + two string headers (10) + shorter (10) + longer cut to 37 bytes +
    // marker (8)
    constexpr std::size_t BUDGET = 70;
    std::vector<std::byte> buffer(dvv::detail::encodedSize(BUDGET, longer, shorter, 7));
    CHECK(buffer.size() <= BUDGET);
    std::byte* end = dvv::detail::encodeArguments(buffer.data(), BUDGET, longer, shorter, 7);
    CHECK(end == buffer.data() + buffer.size());
    std::string out;
    dvv::detail::decodeAndFormat<std::string, std::string, int>(buffer.data(), "{}|{}|{}", out);
    CHECK(out == std::string(37, 'b') + "…[cut]|cccccccccc|7");
}

TEST_CASE("encode: invalid UTF-8 is cut by bytes without reading past it") {
    const std::string latin1 = "caf\xE9 \xE9\xE9\xE9";
    CHECK(dvv::detail::utf8CutPoint(latin1, 5) == 5);
    CHECK(dvv::detail::utf8CutPoint(latin1, 100) == latin1.size());
    CHECK(dvv::detail::utf8CutPoint("\xF0\x9F\x8E\x9E", 2) == 0);  // never splits a sequence
}
