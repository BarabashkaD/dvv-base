#include "dvv/base/error.h"

#include <format>

namespace dvv {

std::string_view errcName(Errc code) noexcept {
    switch (code) {
        case Errc::Io: return "Io";
        case Errc::Format: return "Format";
        case Errc::Unsupported: return "Unsupported";
        case Errc::OutOfMemory: return "OutOfMemory";
        case Errc::DeviceLost: return "DeviceLost";
        case Errc::ShaderCompile: return "ShaderCompile";
        case Errc::Cancelled: return "Cancelled";
        case Errc::Internal: return "Internal";
    }
    return "Unknown";
}

std::string_view sourceFileName(const std::source_location& where) noexcept {
    std::string_view path = where.file_name();
    const auto slash = path.find_last_of("/\\");
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

Error Error::wrap(Errc outer_code, std::string outer_message,
                  std::source_location outer_where) const {
    return Error{outer_code, std::move(outer_message), outer_where,
                 std::make_shared<const Error>(*this)};
}

std::string Error::describe() const {
    std::string text =
        std::format("{}: {} [{}:{}]", errcName(code), message, sourceFileName(where), where.line());
    for (const Error* next = cause.get(); next != nullptr; next = next->cause.get()) {
        text += std::format(" <- {}: {} [{}:{}]", errcName(next->code), next->message,
                            sourceFileName(next->where), next->where.line());
    }
    return text;
}

std::unexpected<Error> makeError(Errc code, std::string message, std::source_location where) {
    return std::unexpected(Error{code, std::move(message), where, nullptr});
}

}  // namespace dvv
