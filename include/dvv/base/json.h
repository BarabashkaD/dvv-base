#pragma once

// Thin dvv::json wrapper over nlohmann/json (spec 2). Only parse, get and dump are the no-throw
// (Result) surface; using Value members directly (operator[], at, get<T> on a Value, ...) can
// throw nlohmann::json::exception.

#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <nlohmann/json.hpp>

#include "dvv/base/error.h"

namespace dvv::json {

using Value = nlohmann::json;

// Errc::Format on malformed input, including empty input and trailing garbage.
[[nodiscard]] Result<Value> parse(std::string_view text);

// indent < 0 gives the compact form. Invalid UTF-8 is replaced with U+FFFD; NaN and infinity are
// written as null. Does not throw on those inputs.
[[nodiscard]] std::string dump(const Value& value, int indent = -1);

// Reads object[key] as T; Errc::Format when the key is missing or has the wrong type. Arithmetic
// types are strict: bool needs a JSON boolean, integers need an integer number that fits T (no
// floats, no bools, no wrap-around), floating point needs a number (not a bool).
template <class T>
[[nodiscard]] Result<T> get(const Value& object, std::string_view key) {
    if (!object.is_object()) {
        return makeError(Errc::Format, "JSON value is not an object");
    }
    const auto found = object.find(key);
    if (found == object.end()) {
        return makeError(Errc::Format, std::format("missing JSON key '{}'", key));
    }
    const auto mismatch = [&key](const char* expected) {
        return makeError(Errc::Format, std::format("JSON key '{}': expected {}", key, expected));
    };
    if constexpr (std::is_same_v<T, bool>) {
        if (!found->is_boolean()) {
            return mismatch("a boolean");
        }
        return found->template get<bool>();
    } else if constexpr (std::is_integral_v<T>) {
        if (!found->is_number_integer()) {
            return mismatch("an integer");
        }
        if (found->is_number_unsigned()) {
            const auto number = found->template get<std::uint64_t>();
            if (!std::in_range<T>(number)) {
                return mismatch("an integer in range");
            }
            return static_cast<T>(number);
        }
        const auto number = found->template get<std::int64_t>();
        if (!std::in_range<T>(number)) {
            return mismatch("an integer in range");
        }
        return static_cast<T>(number);
    } else if constexpr (std::is_floating_point_v<T>) {
        if (!found->is_number()) {
            return mismatch("a number");
        }
        return found->template get<T>();
    } else {
        try {
            return found->template get<T>();
        } catch (const nlohmann::json::exception& error) {
            return makeError(Errc::Format, std::format("JSON key '{}': {}", key, error.what()));
        }
    }
}

}  // namespace dvv::json
