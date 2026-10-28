#include <doctest/doctest.h>

#include <cstdint>
#include <string>

#include "dvv/base/json.h"

TEST_CASE("parse and typed access") {
    const auto value = dvv::json::parse(R"({"width": 1920, "format": "v210"})").value();
    CHECK(dvv::json::get<int>(value, "width").value() == 1920);
    CHECK(dvv::json::get<std::string>(value, "format").value() == "v210");
}

TEST_CASE("invalid JSON is a format error") {
    const auto value = dvv::json::parse("{\"width\": ");
    REQUIRE_FALSE(value.has_value());
    CHECK(value.error().code == dvv::Errc::Format);
}

TEST_CASE("missing keys and wrong types are format errors") {
    const auto value = dvv::json::parse(R"({"width": "wide"})").value();
    const auto missing = dvv::json::get<int>(value, "height");
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().message.find("height") != std::string::npos);
    CHECK(dvv::json::get<int>(value, "width").error().code == dvv::Errc::Format);
    CHECK(dvv::json::get<int>(dvv::json::Value::array(), "x").error().code == dvv::Errc::Format);
}

TEST_CASE("dump") {
    dvv::json::Value value;
    value["a"] = 1;
    CHECK(dvv::json::dump(value) == R"({"a":1})");
    CHECK(dvv::json::dump(value, 2) == "{\n  \"a\": 1\n}");
}

TEST_CASE("get rejects narrowing and wrong-kind numbers") {
    const auto value = dvv::json::parse(
                           R"({"big": 3000000000, "neg": -1, "frac": 1.5, "flag": true, "one": 1,
                               "huge": 18446744073709551615, "minus5": -5, "whole": 7})")
                           .value();
    CHECK(dvv::json::get<int>(value, "big").error().code == dvv::Errc::Format);
    CHECK(dvv::json::get<unsigned>(value, "neg").error().code == dvv::Errc::Format);
    CHECK(dvv::json::get<int>(value, "frac").error().code == dvv::Errc::Format);
    CHECK(dvv::json::get<int>(value, "flag").error().code == dvv::Errc::Format);
    CHECK(dvv::json::get<bool>(value, "one").error().code == dvv::Errc::Format);
    CHECK(dvv::json::get<double>(value, "flag").error().code == dvv::Errc::Format);
    CHECK(dvv::json::get<std::int64_t>(value, "huge").error().code == dvv::Errc::Format);
    CHECK(dvv::json::get<std::uint64_t>(value, "huge").value() == 18446744073709551615ULL);
    CHECK(dvv::json::get<std::int64_t>(value, "minus5").value() == -5);
    CHECK(dvv::json::get<double>(value, "whole").value() == 7.0);
    CHECK(dvv::json::get<bool>(value, "flag").value());
}

TEST_CASE("type mismatch message names the key") {
    const auto value = dvv::json::parse(R"({"width": "wide", "big": 3000000000})").value();
    const auto text = dvv::json::get<int>(value, "width");
    REQUIRE_FALSE(text.has_value());
    CHECK(text.error().message.find("width") != std::string::npos);
    const auto big = dvv::json::get<int>(value, "big");
    REQUIRE_FALSE(big.has_value());
    CHECK(big.error().message.find("big") != std::string::npos);
}

TEST_CASE("dump replaces invalid UTF-8") {
    dvv::json::Value value;
    value["s"] = std::string("a") + std::string(1, static_cast<char>(0xFF)) + "b";
    std::string text;
    CHECK_NOTHROW(text = dvv::json::dump(value));
    CHECK(text ==
          "{\"s\":\"a\xEF\xBF\xBD"
          "b\"}");
}

TEST_CASE("parse rejects empty input and trailing garbage") {
    CHECK(dvv::json::parse("").error().code == dvv::Errc::Format);
    CHECK(dvv::json::parse("{} x").error().code == dvv::Errc::Format);
}
