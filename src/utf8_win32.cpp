#include "dvv/base/utf8_win32.h"

#include "utf8_detail.h"

namespace dvv {

std::optional<std::string> utf8FromWide(std::wstring_view text) {
    return detail::utf8FromUtf16<wchar_t>(text, detail::BadUnits::Reject);
}

std::wstring wideFromUtf8(std::string_view utf8) {
    return detail::utf16FromUtf8<wchar_t>(utf8);
}

}  // namespace dvv
