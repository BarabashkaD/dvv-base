#pragma once

// Argument encoding of the queued logger: a call's arguments are copied into a record as a tag byte
// and the value, and formatted later by decodeAndFormat on the log thread.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <iterator>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>

namespace dvv::detail {

inline constexpr std::size_t LOG_RING_BYTES = std::size_t{4} << 20U;
inline constexpr std::size_t LOG_MAX_RECORD_BYTES = LOG_RING_BYTES / 16;

enum class LogArgTag : std::uint8_t {
    Bool,
    Char,
    I8,
    U8,
    I16,
    U16,
    I32,
    U32,
    I64,
    U64,
    F32,
    F64,
    F80,  // long double, stored as its sizeof bytes
    String,
    Pointer
};

using LogFormatFn = void (*)(const std::byte* arguments, std::string_view format, std::string& out);

// The marker that ends a cut string: "…" (3 bytes) and "[cut]" (5 bytes).
// Spelled in bytes, so it does not depend on the source character set a consumer compiles with.
inline constexpr std::string_view LOG_CUT_MARKER = "\xE2\x80\xA6[cut]";
static_assert(LOG_CUT_MARKER.size() == 8);
inline constexpr std::size_t LOG_STRING_HEADER_BYTES = 1 + sizeof(std::uint32_t);

namespace log_encode_impl {

template <class T>
inline constexpr bool IS_TEXT_ARRAY =
    std::is_array_v<T> && std::is_same_v<std::remove_extent_t<T>, char>;

template <class T>
inline constexpr bool IS_STRING =
    std::is_same_v<T, std::string> || std::is_same_v<T, std::string_view> ||
    std::is_same_v<T, const char*> || std::is_same_v<T, char*> || IS_TEXT_ARRAY<T>;

template <class T>
inline constexpr bool IS_POINTER =
    std::is_same_v<T, const void*> || std::is_same_v<T, void*> || std::is_same_v<T, std::nullptr_t>;

template <class T>
inline constexpr bool IS_SIZED_INTEGER =
    std::is_integral_v<T> && !std::is_same_v<T, bool> && !std::is_same_v<T, char> &&
    !std::is_same_v<T, wchar_t> && !std::is_same_v<T, char8_t> && !std::is_same_v<T, char16_t> &&
    !std::is_same_v<T, char32_t> &&
    (sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8);

template <class T>
inline constexpr bool IS_FLOAT =
    std::is_same_v<T, float> || std::is_same_v<T, double> || std::is_same_v<T, long double>;

template <class T>
consteval LogArgTag tagOf() {
    if constexpr (std::is_same_v<T, bool>) {
        return LogArgTag::Bool;
    } else if constexpr (std::is_same_v<T, char>) {
        return LogArgTag::Char;
    } else if constexpr (IS_SIZED_INTEGER<T>) {
        constexpr bool SIGNED = std::is_signed_v<T>;
        if constexpr (sizeof(T) == 1) {
            return SIGNED ? LogArgTag::I8 : LogArgTag::U8;
        } else if constexpr (sizeof(T) == 2) {
            return SIGNED ? LogArgTag::I16 : LogArgTag::U16;
        } else if constexpr (sizeof(T) == 4) {
            return SIGNED ? LogArgTag::I32 : LogArgTag::U32;
        } else {
            return SIGNED ? LogArgTag::I64 : LogArgTag::U64;
        }
    } else if constexpr (std::is_same_v<T, float>) {
        return LogArgTag::F32;
    } else if constexpr (std::is_same_v<T, double>) {
        return LogArgTag::F64;
    } else if constexpr (std::is_same_v<T, long double>) {
        return LogArgTag::F80;
    } else if constexpr (IS_STRING<T>) {
        return LogArgTag::String;
    } else {
        return LogArgTag::Pointer;
    }
}

// The type a decoded argument has: the exact stored type, strings as views.
template <class Arg>
using Stored = std::conditional_t<IS_STRING<std::remove_cvref_t<Arg>>, std::string_view,
                                  std::conditional_t<IS_POINTER<std::remove_cvref_t<Arg>>,
                                                     const void*, std::remove_cvref_t<Arg>>>;

template <class Arg>
inline constexpr bool IS_STRING_ARG = IS_STRING<std::remove_cvref_t<Arg>>;

// The text of a string argument; a null pointer is "(null)", an array ends at its first NUL.
template <class T>
std::string_view textOf(const T& value) noexcept {
    if constexpr (IS_TEXT_ARRAY<T>) {
        const char* end = static_cast<const char*>(std::memchr(value, '\0', sizeof(T)));
        return {value, end == nullptr ? sizeof(T) : static_cast<std::size_t>(end - value)};
    } else if constexpr (std::is_same_v<T, const char*> || std::is_same_v<T, char*>) {
        return value == nullptr ? std::string_view{"(null)"} : std::string_view{value};
    } else {
        return std::string_view{value};
    }
}

[[nodiscard]] inline bool isContinuation(char byte) noexcept {
    return (static_cast<unsigned char>(byte) & 0xC0U) == 0x80U;
}

template <class T>
void put(std::byte*& out, const T& value) noexcept {
    std::memcpy(out, static_cast<const void*>(&value), sizeof(T));
    out += sizeof(T);
}

template <class T>
T take(const std::byte*& in) noexcept {
    T value;
    std::memcpy(static_cast<void*>(&value), in, sizeof(T));
    in += sizeof(T);
    return value;
}

}  // namespace log_encode_impl

template <class T>
inline constexpr bool LOG_DEFERRABLE =
    log_encode_impl::IS_STRING<std::remove_cvref_t<T>> ||
    log_encode_impl::IS_POINTER<std::remove_cvref_t<T>> ||
    log_encode_impl::IS_FLOAT<std::remove_cvref_t<T>> ||
    std::is_same_v<std::remove_cvref_t<T>, bool> || std::is_same_v<std::remove_cvref_t<T>, char> ||
    log_encode_impl::IS_SIZED_INTEGER<std::remove_cvref_t<T>>;

template <class... Args>
inline constexpr bool LOG_ALL_DEFERRABLE = (LOG_DEFERRABLE<Args> && ...);

// The largest n <= limit that does not split a multi-byte sequence; invalid bytes count as single
// bytes. Reads only text[0..limit] and only when limit < text.size().
[[nodiscard]] inline std::size_t utf8CutPoint(std::string_view text, std::size_t limit) noexcept {
    if (limit >= text.size()) {
        return text.size();
    }
    if (!log_encode_impl::isContinuation(text[limit])) {
        return limit;
    }
    // text[limit] continues a sequence: walk back over at most three continuation bytes to its
    // lead.
    std::size_t start = limit;
    while (start > 0 && limit - start < 3) {
        --start;
        if (!log_encode_impl::isContinuation(text[start])) {
            break;
        }
    }
    const auto lead = static_cast<unsigned char>(text[start]);
    std::size_t length = 1;
    if (lead >= 0xF0U && lead <= 0xF4U) {
        length = 4;
    } else if (lead >= 0xE0U && lead < 0xF0U) {
        length = 3;
    } else if (lead >= 0xC2U && lead < 0xE0U) {
        length = 2;
    }
    return start + length > limit && length > 1 ? start : limit;
}

namespace log_encode_impl {

// What a string of the given length takes when at most `keep` text bytes are kept before the
// marker.
inline std::size_t cutSize(std::string_view text, std::size_t keep) noexcept {
    return std::min(text.size(), utf8CutPoint(text, keep) + LOG_CUT_MARKER.size());
}

// The Args' strings and the bytes everything else takes; `keep` is the cut limit for the strings.
template <class... Args>
class Plan {
public:
    [[nodiscard]] std::string_view string(std::size_t index) const noexcept {
        return strings_[index];
    }
    [[nodiscard]] std::size_t keep() const noexcept { return keep_; }

    [[nodiscard]] std::size_t total(std::size_t limit) const noexcept {
        std::size_t bytes = fixed_bytes_;
        for (std::size_t i = 0; i < string_count_; ++i) {
            bytes += cutSize(strings_[i], limit);
        }
        return bytes;
    }

    Plan(std::size_t budget, const Args&... args) noexcept {
        (add(args), ...);
        std::size_t longest = 0;
        for (std::size_t i = 0; i < string_count_; ++i) {
            longest = std::max(longest, strings_[i].size());
        }
        if (total(longest) <= budget) {
            return;  // everything fits: no cut
        }
        // Largest keep whose total fits; keep_ = 0 when even that does not (the budget is too
        // small).
        std::size_t low = 0;
        std::size_t high = longest;  // total(high) > budget
        if (total(0) > budget) {
            keep_ = 0;
            return;
        }
        while (high - low > 1) {
            const std::size_t mid = low + (high - low) / 2;
            (total(mid) <= budget ? low : high) = mid;
        }
        keep_ = low;
    }

private:
    std::array<std::string_view, (0 + ... + (IS_STRING_ARG<Args> ? 1 : 0)) + 1> strings_{};
    std::size_t string_count_ = 0;
    std::size_t fixed_bytes_ = 0;  // tags, scalar values and the string headers
    std::size_t keep_ = static_cast<std::size_t>(-1);

    template <class T>
    void add(const T& value) noexcept {
        using Plain = std::remove_cvref_t<T>;
        if constexpr (IS_STRING<Plain>) {
            strings_[string_count_++] = textOf<Plain>(value);
            fixed_bytes_ += LOG_STRING_HEADER_BYTES;
        } else {
            fixed_bytes_ += 1 + sizeof(Stored<T>);
        }
    }
};

}  // namespace log_encode_impl

// The bytes encodeArguments writes for these arguments under `budget` (the argument bytes allowed,
// the cut marker included). Smaller than the budget unless even fully cut strings do not fit.
template <class... Args>
[[nodiscard]] std::size_t encodedSize(std::size_t budget, const Args&... args) noexcept {
    const log_encode_impl::Plan<Args...> plan{budget, args...};
    return plan.total(plan.keep());
}

template <class... Args>
std::byte* encodeArguments(std::byte* out, std::size_t budget, const Args&... args) noexcept {
    namespace impl = log_encode_impl;
    const impl::Plan<Args...> plan{budget, args...};
    std::size_t string_index = 0;
    [[maybe_unused]] const auto encode_one = [&](const auto& value) noexcept {
        using Plain = std::remove_cvref_t<decltype(value)>;
        *out++ = static_cast<std::byte>(impl::tagOf<Plain>());
        if constexpr (impl::IS_STRING<Plain>) {
            const std::string_view text = plan.string(string_index++);
            const bool cut = impl::cutSize(text, plan.keep()) < text.size();
            const std::size_t kept = cut ? utf8CutPoint(text, plan.keep()) : text.size();
            impl::put(out, static_cast<std::uint32_t>(kept + (cut ? LOG_CUT_MARKER.size() : 0)));
            std::memcpy(out, text.data(), kept);
            out += kept;
            if (cut) {
                std::memcpy(out, LOG_CUT_MARKER.data(), LOG_CUT_MARKER.size());
                out += LOG_CUT_MARKER.size();
            }
        } else if constexpr (impl::IS_POINTER<Plain>) {
            impl::put(out, static_cast<const void*>(value));
        } else {
            impl::put(out, value);
        }
    };
    (encode_one(args), ...);
    return out;
}

// Decodes arguments written by encodeArguments for exactly these types and formats them in one
// frame, so no format_args outlives the values it refers to.
template <class... Args>
void decodeAndFormat(const std::byte* arguments, std::string_view format, std::string& out) {
    namespace impl = log_encode_impl;
    [[maybe_unused]] const auto decode_one =
        [&arguments]<class Arg>() noexcept -> impl::Stored<Arg> {
        ++arguments;  // the tag
        if constexpr (impl::IS_STRING_ARG<Arg>) {
            const auto length = impl::take<std::uint32_t>(arguments);
            const std::string_view text{reinterpret_cast<const char*>(arguments), length};
            arguments += length;
            return text;
        } else {
            return impl::take<impl::Stored<Arg>>(arguments);
        }
    };
    // A braced list is evaluated left to right, which is the stored order.
    std::tuple<impl::Stored<Args>...> locals { decode_one.template operator()<Args>()... };
    std::apply(
        [&](auto&... values) {
            std::vformat_to(std::back_inserter(out), format, std::make_format_args(values...));
        },
        locals);
}

}  // namespace dvv::detail
