#pragma once
// Private helpers of dvv/base/utf8.h (UTF-8 validation, replacement, UTF-16 conversion).
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace dvv::detail {
// Table 3-7: the length of the well-formed sequence that starts at text[i], or 0 if none does.
[[nodiscard]] std::size_t wellFormedLength(std::string_view text, std::size_t i) noexcept;
// Where wellFormedLength is 0: the length (1 to 3) of the maximal subpart that starts at text[i]
// (Unicode 16.0 section 3.9, "U+FFFD Substitution of Maximal Subparts").
[[nodiscard]] std::size_t illFormedLength(std::string_view text, std::size_t i) noexcept;
// Bytes to well-formed UTF-8: each maximal ill-formed subpart becomes U+FFFD.
[[nodiscard]] std::string replaceIllFormed(std::string_view bytes);

enum class BadUnits : std::uint8_t { Replace, Reject };

// UTF-16 to UTF-8. Replace: each unpaired surrogate becomes U+FFFD (never nullopt).
// Reject: nullopt on the first unpaired surrogate.
// Defined in this header for char16_t and wchar_t.
template <class Unit>
[[nodiscard]] std::optional<std::string> utf8FromUtf16(std::basic_string_view<Unit> text,
                                                       BadUnits bad) {
    std::string out;
    out.reserve(text.size());
    const auto put = [&out](std::uint32_t cp) {
        if (cp < 0x80U) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800U) {
            out.push_back(static_cast<char>(0xC0U | (cp >> 6U)));
            out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
        } else if (cp < 0x10000U) {
            out.push_back(static_cast<char>(0xE0U | (cp >> 12U)));
            out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
        } else {
            out.push_back(static_cast<char>(0xF0U | (cp >> 18U)));
            out.push_back(static_cast<char>(0x80U | ((cp >> 12U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
        }
    };
    const auto unit = [&text](std::size_t at) { return static_cast<std::uint32_t>(text[at]); };
    for (std::size_t i = 0; i < text.size(); ++i) {
        const std::uint32_t u = unit(i);
        if (u >= 0xD800U && u <= 0xDBFFU && i + 1 < text.size() && unit(i + 1) >= 0xDC00U &&
            unit(i + 1) <= 0xDFFFU) {
            put(0x10000U + ((u - 0xD800U) << 10U) + (unit(i + 1) - 0xDC00U));
            ++i;
        } else if (u >= 0xD800U && u <= 0xDFFFU) {
            if (bad == BadUnits::Reject) {
                return std::nullopt;
            }
            put(0xFFFDU);
        } else {
            put(u);
        }
    }
    return out;
}

// UTF-8 to UTF-16: each maximal ill-formed subpart becomes U+FFFD.
template <class Unit>
[[nodiscard]] std::basic_string<Unit> utf16FromUtf8(std::string_view text) {
    std::basic_string<Unit> out;
    out.reserve(text.size());
    const auto byte = [&text](std::size_t at) {
        return static_cast<std::uint32_t>(static_cast<std::uint8_t>(text[at]));
    };
    std::size_t i = 0;
    while (i < text.size()) {
        const std::size_t length = wellFormedLength(text, i);
        if (length == 0) {
            out.push_back(static_cast<Unit>(0xFFFDU));
            i += illFormedLength(text, i);
            continue;
        }
        std::uint32_t cp = byte(i);
        if (length == 2) {
            cp = ((cp & 0x1FU) << 6U) | (byte(i + 1) & 0x3FU);
        } else if (length == 3) {
            cp = ((cp & 0x0FU) << 12U) | ((byte(i + 1) & 0x3FU) << 6U) | (byte(i + 2) & 0x3FU);
        } else if (length == 4) {
            cp = ((cp & 0x07U) << 18U) | ((byte(i + 1) & 0x3FU) << 12U) |
                 ((byte(i + 2) & 0x3FU) << 6U) | (byte(i + 3) & 0x3FU);
        }
        if (cp >= 0x10000U) {
            const std::uint32_t v = cp - 0x10000U;
            out.push_back(static_cast<Unit>(0xD800U + (v >> 10U)));
            out.push_back(static_cast<Unit>(0xDC00U + (v & 0x3FFU)));
        } else {
            out.push_back(static_cast<Unit>(cp));
        }
        i += length;
    }
    return out;
}

// The native forms. Overload resolution on path::string_type picks the platform's pair.
[[nodiscard]] inline std::string textOfNative(const std::string& native) {
    return native;
}
[[nodiscard]] inline std::string textOfNative(const std::wstring& native) {
    return *utf8FromUtf16<wchar_t>(native, BadUnits::Replace);
}
[[nodiscard]] inline std::string displayOfNative(const std::string& native) {
    return replaceIllFormed(native);
}
[[nodiscard]] inline std::string displayOfNative(const std::wstring& native) {
    return *utf8FromUtf16<wchar_t>(native, BadUnits::Replace);
}
inline void nativeOfText(std::string_view text, std::string& native) {
    native.assign(text);
}
inline void nativeOfText(std::string_view text, std::wstring& native) {
    native = utf16FromUtf8<wchar_t>(text);
}
}  // namespace dvv::detail
