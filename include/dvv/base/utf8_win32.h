// include/dvv/base/utf8_win32.h — Windows only: wchar_t is UTF-16 there. Include it only in
// Windows code: a file with a Windows suffix, or the `_WIN32` include group of a shared file
// until that code moves into a per-OS file.
#pragma once
#if !defined(_WIN32)
#error "dvv/base/utf8_win32.h is Windows only"
#endif
#include <optional>
#include <string>
#include <string_view>
namespace dvv {
// Strict UTF-16 → UTF-8: nullopt for an unpaired surrogate; "" for empty input.
// Never throws std::system_error; may throw std::bad_alloc.
[[nodiscard]] std::optional<std::string> utf8FromWide(std::wstring_view text);
// UTF-8 → UTF-16 with pathFromText's Windows rule: each maximal ill-formed subpart becomes
// U+FFFD; never fails. A caller that must reject bad input checks isValidUtf8 first.
[[nodiscard]] std::wstring wideFromUtf8(std::string_view utf8);
}  // namespace dvv
