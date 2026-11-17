#pragma once

// UTF-8 checks and UTF-8 text <-> std::filesystem::path. path(std::string) and path::string() use
// the ANSI code page on Windows; these go through char8_t on every OS. Private to dvv-base and its
// tests.

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace dvv::detail {

// Well-formed UTF-8 (RFC 3629): no overlong forms, no surrogates, nothing above U+10FFFF.
[[nodiscard]] inline bool isValidUtf8(std::string_view text) noexcept {
    std::size_t i = 0;
    while (i < text.size()) {
        const auto lead = static_cast<unsigned char>(text[i]);
        if (lead < 0x80) {
            ++i;
            continue;
        }
        std::size_t length = 0;
        char32_t code = 0;
        char32_t minimum = 0;
        if ((lead & 0xE0) == 0xC0) {
            length = 2;
            code = lead & 0x1F;
            minimum = 0x80;
        } else if ((lead & 0xF0) == 0xE0) {
            length = 3;
            code = lead & 0x0F;
            minimum = 0x800;
        } else if ((lead & 0xF8) == 0xF0) {
            length = 4;
            code = lead & 0x07;
            minimum = 0x10000;
        } else {
            return false;
        }
        if (text.size() - i < length) {
            return false;
        }
        for (std::size_t k = 1; k < length; ++k) {
            const auto next = static_cast<unsigned char>(text[i + k]);
            if ((next & 0xC0) != 0x80) {
                return false;
            }
            code = (code << 6) | (next & 0x3F);
        }
        if (code < minimum || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) {
            return false;
        }
        i += length;
    }
    return true;
}

[[nodiscard]] inline std::string utf8(const std::filesystem::path& path) {
    const std::u8string units = path.u8string();
    std::string text(units.size(), '\0');
    std::ranges::transform(units, text.begin(),
                           [](char8_t unit) { return static_cast<char>(unit); });
    return text;
}

[[nodiscard]] inline std::filesystem::path pathFromUtf8(std::string_view text) {
    std::u8string units(text.size(), u8'\0');
    std::ranges::transform(text, units.begin(), [](char c) { return static_cast<char8_t>(c); });
    return {units};
}

}  // namespace dvv::detail
