#include <doctest/doctest.h>

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <set>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "dvv/base/error.h"

namespace {

dvv::Result<int> parseNumber(std::string_view text) {
    int value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return dvv::makeError(dvv::Errc::Format, "not a number");
    }
    return value;
}

dvv::Result<int> doubled(std::string_view text) {
    DVV_TRY(const int value, parseNumber(text));
    return value * 2;
}

dvv::Result<void> mustBePositive(int value) {
    if (value <= 0) {
        return dvv::makeError(dvv::Errc::Format, "not positive");
    }
    return {};
}

dvv::Result<int> checkedDouble(int value) {
    DVV_TRY_VOID(mustBePositive(value));
    return value * 2;
}

}  // namespace

TEST_CASE("Errc names") {
    CHECK(dvv::errcName(dvv::Errc::Io) == "Io");
    CHECK(dvv::errcName(dvv::Errc::Format) == "Format");
    CHECK(dvv::errcName(dvv::Errc::Cancelled) == "Cancelled");
    CHECK(dvv::errcName(dvv::Errc::DeviceLost) == "DeviceLost");
    CHECK(dvv::errcName(dvv::Errc::Internal) == "Internal");
}

TEST_CASE("every Errc value up to Internal has its own name") {
    std::set<std::string_view> names;
    for (std::uint8_t value = 0; value <= std::to_underlying(dvv::Errc::Internal); ++value) {
        const std::string_view name = dvv::errcName(static_cast<dvv::Errc>(value));
        CHECK(name != "Unknown");
        names.insert(name);
    }
    CHECK(names.size() == std::size_t{std::to_underlying(dvv::Errc::Internal)} + 1);
}

TEST_CASE("makeError fills code, message and location") {
    const int line = __LINE__ + 1;
    const dvv::Result<int> result = dvv::makeError(dvv::Errc::Io, "read failed");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == dvv::Errc::Io);
    CHECK(result.error().message == "read failed");
    CHECK(result.error().where.line() == static_cast<std::uint_least32_t>(line));
    CHECK(result.error().cause == nullptr);
}

TEST_CASE("wrap chains errors and describe shows the chain with file names only") {
    const dvv::Error inner{dvv::Errc::Io, "read failed", std::source_location::current(), nullptr};
    const dvv::Error outer = inner.wrap(dvv::Errc::Format, "bad header");
    REQUIRE(outer.cause != nullptr);
    CHECK(outer.cause->message == "read failed");
    const std::string text = outer.describe();
    CHECK(text.starts_with("Format: bad header [test_error.cpp:"));
    CHECK(text.find(" <- Io: read failed [test_error.cpp:") != std::string::npos);
    CHECK(text.find('/') == std::string::npos);
    CHECK(text.find('\\') == std::string::npos);
}

TEST_CASE("DVV_TRY returns the value or the error") {
    CHECK(doubled("21").value() == 42);
    const auto failed = doubled("x");
    REQUIRE_FALSE(failed.has_value());
    CHECK(failed.error().message == "not a number");
}

TEST_CASE("DVV_TRY_VOID returns the error early") {
    CHECK(checkedDouble(4).value() == 8);
    CHECK(checkedDouble(-1).error().message == "not positive");
}

TEST_CASE("describeCurrentException names the exception being handled") {
    int handled = 0;
    try {
        throw std::runtime_error("boom");
    } catch (...) {
        ++handled;
        CHECK(dvv::describeCurrentException() == "boom");
    }
    try {
        (void)dvv::Result<int>(dvv::makeError(dvv::Errc::Io, "read failed")).value();
    } catch (...) {
        ++handled;
        CHECK(dvv::describeCurrentException().starts_with("Io: read failed [test_error.cpp:"));
    }
    try {
        throw 42;
    } catch (...) {
        ++handled;
        CHECK(dvv::describeCurrentException() == "unknown exception");
    }
    CHECK(handled == 3);
    CHECK(dvv::describeCurrentException() == "no exception");
}

TEST_CASE("describeCurrentException gives the whole chain of a failed Result") {
    const dvv::Error outer =
        dvv::Error{dvv::Errc::Io, "read failed", std::source_location::current(), nullptr}.wrap(
            dvv::Errc::Format, "bad header");
    bool handled = false;
    try {
        (void)dvv::Result<int>(std::unexpected(outer)).value();
    } catch (...) {
        handled = true;
        const std::string text = dvv::describeCurrentException();
        CHECK(text == outer.describe());
        CHECK(text.find(" <- Io: read failed [test_error.cpp:") != std::string::npos);
    }
    CHECK(handled);
}

TEST_CASE("describeCurrentException gives what() of another bad_expected_access") {
    bool handled = false;
    try {
        (void)std::expected<int, int>(std::unexpected(7)).value();
    } catch (...) {
        handled = true;
        const std::string text = dvv::describeCurrentException();
        CHECK(text == std::string(std::bad_expected_access<int>(7).what()));
        CHECK(text != "unknown exception");
    }
    CHECK(handled);
}
