#pragma once

// ASCII letter test and case folding, independent of the C locale. Only A-Z and a-z are letters;
// every other byte, UTF-8 sequences included, passes unchanged.

#include <cstddef>
#include <string_view>

namespace dvv {

[[nodiscard]] constexpr bool isAsciiLetter(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

[[nodiscard]] constexpr char asciiLower(char c) noexcept {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] constexpr char asciiUpper(char c) noexcept {
    return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
}

// a == b with A-Z and a-z compared without case.
[[nodiscard]] constexpr bool equalsIgnoreAscii(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (asciiLower(a[i]) != asciiLower(b[i])) {
            return false;
        }
    }
    return true;
}

}  // namespace dvv
