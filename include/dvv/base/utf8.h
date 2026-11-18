#pragma once
#include <filesystem>
#include <string>
#include <string_view>

namespace dvv {
// Well-formed UTF-8 (RFC 3629, Unicode Table 3-7): no overlong forms, no surrogates, nothing
// above U+10FFFF, no truncated sequence.
[[nodiscard]] bool isValidUtf8(std::string_view text) noexcept;

// Path text, the round-trip form: pathFromText(pathText(p)) == p. On POSIX the path's own bytes,
// unchanged; they need not be UTF-8. On Windows the UTF-8 form of the UTF-16 name; an unpaired
// surrogate becomes U+FFFD, the one lossy case. Use it where the text names the file again (a
// URI, a library name, a hash key, a stored setting, an argument for the OS or FFmpeg). Never
// show it: use pathDisplayText. Never throws std::system_error (it may throw std::bad_alloc).
[[nodiscard]] std::string pathText(const std::filesystem::path& path);

// The path that `text` names: the inverse of pathText, and the path of any UTF-8 text. On POSIX
// the bytes become the path unchanged. On Windows the UTF-8 becomes UTF-16, never through the
// ANSI code page, and each maximal ill-formed subpart becomes U+FFFD. Never throws
// std::system_error (it may throw std::bad_alloc).
[[nodiscard]] std::filesystem::path pathFromText(std::string_view text);

// Display text: well-formed UTF-8 on every OS, for messages, logs, UI text and name grammars.
// Each maximal ill-formed subpart (POSIX) or unpaired surrogate (Windows) becomes U+FFFD. Lossy:
// never turn it back into a path. Never throws std::system_error (it may throw std::bad_alloc).
[[nodiscard]] std::string pathDisplayText(const std::filesystem::path& path);
}  // namespace dvv
