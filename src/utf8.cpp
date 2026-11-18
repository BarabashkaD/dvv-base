#include "dvv/base/utf8.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "utf8_detail.h"

namespace dvv::detail {

namespace {

// Table 3-7 row of a lead byte: the sequence length (0 if the byte starts none) and the allowed
// range of the second byte; every later byte is 80..BF.
struct LeadRule {
    std::size_t length;
    std::uint8_t low;
    std::uint8_t high;
};

LeadRule leadRule(std::uint8_t lead) noexcept {
    if (lead >= 0xC2U && lead <= 0xDFU) {
        return {2, 0x80U, 0xBFU};
    }
    if (lead == 0xE0U) {
        return {3, 0xA0U, 0xBFU};
    }
    if (lead == 0xEDU) {
        return {3, 0x80U, 0x9FU};  // no surrogates
    }
    if (lead >= 0xE1U && lead <= 0xEFU) {
        return {3, 0x80U, 0xBFU};
    }
    if (lead == 0xF0U) {
        return {4, 0x90U, 0xBFU};
    }
    if (lead >= 0xF1U && lead <= 0xF3U) {
        return {4, 0x80U, 0xBFU};
    }
    if (lead == 0xF4U) {
        return {4, 0x80U, 0x8FU};  // nothing above U+10FFFF
    }
    return {0, 0x80U, 0xBFU};  // 80..C1, F5..FF
}

// The number of bytes from text[i] (at most rule.length) that match the rule.
std::size_t matchingBytes(std::string_view text, std::size_t i, const LeadRule& rule) noexcept {
    std::size_t used = 1;
    while (used < rule.length && i + used < text.size()) {
        const auto next = static_cast<std::uint8_t>(text[i + used]);
        const bool second = used == 1;
        if (next < (second ? rule.low : 0x80U) || next > (second ? rule.high : 0xBFU)) {
            break;
        }
        ++used;
    }
    return used;
}

}  // namespace

// The length of the well-formed UTF-8 sequence starting at text[i] (Unicode Table 3-7), or 0 if
// the byte there does not start one.
std::size_t wellFormedLength(std::string_view text, std::size_t i) noexcept {
    const auto lead = static_cast<std::uint8_t>(text[i]);
    if (lead <= 0x7FU) {
        return 1;
    }
    const LeadRule rule = leadRule(lead);
    if (rule.length == 0 || matchingBytes(text, i, rule) != rule.length) {
        return 0;  // not a lead byte, or truncated or a bad continuation
    }
    return rule.length;
}

// Where wellFormedLength is 0: the maximal subpart that starts at text[i] (1 for a byte that
// starts no sequence).
std::size_t illFormedLength(std::string_view text, std::size_t i) noexcept {
    return matchingBytes(text, i, leadRule(static_cast<std::uint8_t>(text[i])));
}

std::string replaceIllFormed(std::string_view bytes) {
    std::string out;
    out.reserve(bytes.size());
    std::size_t i = 0;
    while (i < bytes.size()) {
        const std::size_t length = wellFormedLength(bytes, i);
        if (length != 0) {
            out.append(bytes.substr(i, length));
            i += length;
        } else {
            out.append("\xEF\xBF\xBD");
            i += illFormedLength(bytes, i);
        }
    }
    return out;
}

}  // namespace dvv::detail

namespace dvv {

bool isValidUtf8(std::string_view text) noexcept {
    for (std::size_t i = 0; i < text.size();) {
        const std::size_t length = detail::wellFormedLength(text, i);
        if (length == 0) {
            return false;
        }
        i += length;
    }
    return true;
}

std::string pathText(const std::filesystem::path& path) {
    return detail::textOfNative(path.native());
}

std::string pathDisplayText(const std::filesystem::path& path) {
    return detail::displayOfNative(path.native());
}

std::filesystem::path pathFromText(std::string_view text) {
    std::filesystem::path::string_type native;
    detail::nativeOfText(text, native);
    return std::filesystem::path(std::move(native));
}

}  // namespace dvv
