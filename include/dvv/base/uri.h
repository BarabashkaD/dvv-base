#pragma once

// Small RFC 3986 subset for source URIs (spec 7.2).
//   clip.yuv, X:\media\clip.yuv, /data/clip.yuv   bare paths mean file: (path not percent-decoded)
//   file:clip_1920x1080.yuv?fmt=v210&fps=50        path + query parameters (percent-decoded)
//   file:///X:/media/a%20b.yuv                     empty or "localhost" authority only
//   synthetic:gradient?w=33&h=17&fmt=v210          like file:
//   lavfi:testsrc2=s=3840x2160:r=60                any other scheme is opaque: body kept unchanged
//
// Windows drive letters: after percent-decoding, a path of the form "/X:" followed by '/', '\' or
// the end (X an ASCII letter) loses its leading '/'. This applies to every file: form
// (file:/X:/a, file:///X:/a, file:///X%3A/a) on every platform. Bare paths are never changed.
//
// Query rules (hierarchical schemes and bare paths):
//   - empty segments ("&&", a trailing "&") are skipped;
//   - "a" and "a=" both give the value "";
//   - "a=b=c" gives the value "b=c" (only the first '=' separates);
//   - an empty name ("?=v") is Errc::Format;
//   - duplicate names are kept in order and param() returns the first;
//   - '+' is not decoded (it stays '+').
//
// Bare paths are not percent-decoded, but their query (after '?') is.
//
// Decoded values are untrusted input. A decoded path or parameter may contain '/', '\', ".." and
// control characters; callers must validate them before using them as file names or writing them
// to logs. A raw NUL byte in the input and an encoded NUL (%00) are rejected with Errc::Format.

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "dvv/base/error.h"

namespace dvv {

struct Uri {
    std::string scheme;  // ASCII lower case; "file" for bare paths
    // Opaque schemes only (for example lavfi:): everything after "scheme:", unchanged and never
    // empty. Empty for file: and synthetic:, where path and params carry the whole meaning.
    std::string body;
    std::string path;  // decoded path for file: and synthetic:, empty for opaque schemes
    std::vector<std::pair<std::string, std::string>> params;  // decoded, in order, names non-empty

    // The view points into this object: lvalues only, so a temporary Uri cannot dangle.
    [[nodiscard]] std::optional<std::string_view> param(std::string_view name) const&;
    [[nodiscard]] std::optional<std::string_view> param(std::string_view name) const&& = delete;
    bool operator==(const Uri&) const = default;
};

[[nodiscard]] Result<Uri> parseUri(std::string_view text);

// Text that parses back to the same value: for every x where parseUri(x) succeeds with a
// hierarchical scheme, parseUri(formatUri(u)) == u with u = parseUri(x).value(), including
// non-canonical x. A path starting with "//" is written after an empty authority ("file://" +
// path) so that it cannot be read as an authority. A bare path of the form "/X:/..." cannot be
// written as file:, because the drive rule would drop its '/'; it is written as a bare path.
// Query values keep '/', ':', '@' and '\' unencoded; '&', '=', '#', '%', '+', spaces, controls and
// non-ASCII are percent-encoded.
[[nodiscard]] std::string formatUri(const Uri& uri);

// Rejects truncated or non-hex escapes and an encoded NUL (%00) with Errc::Format.
[[nodiscard]] Result<std::string> percentDecode(std::string_view text);
// Encodes every byte except ASCII letters, digits, "-._~" and the characters in keep. '%' is always
// encoded, even when keep contains it, so the output always decodes back to the input.
[[nodiscard]] std::string percentEncode(std::string_view text, std::string_view keep = "/:\\");

}  // namespace dvv
