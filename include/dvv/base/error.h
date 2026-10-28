#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <source_location>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace dvv {

enum class Errc : std::uint8_t {
    Io,
    Format,
    Unsupported,
    OutOfMemory,
    DeviceLost,
    ShaderCompile,
    Cancelled,
    Internal,
};

[[nodiscard]] std::string_view errcName(Errc code) noexcept;

// File name without directories, for messages and logs.
[[nodiscard]] std::string_view sourceFileName(const std::source_location& where) noexcept;

struct Error {
    Errc code = Errc::Internal;
    std::string message;  // for people; includes file/frame/component context
    std::source_location where;
    std::shared_ptr<const Error> cause;  // chain

    // Returns a new error whose cause is this one.
    [[nodiscard]] Error wrap(
        Errc outer_code, std::string outer_message,
        std::source_location outer_where = std::source_location::current()) const;

    // "Format: bad header [uri.cpp:42] <- Io: read failed [file.cpp:10]"
    [[nodiscard]] std::string describe() const;
};

// Promise::failWith and the task layer move errors in noexcept functions.
static_assert(std::is_nothrow_move_constructible_v<Error>);

template <class T = void>
using Result = std::expected<T, Error>;

[[nodiscard]] std::unexpected<Error> makeError(
    Errc code, std::string message, std::source_location where = std::source_location::current());

}  // namespace dvv

#define DVV_DETAIL_CONCAT_INNER(a, b) a##b
#define DVV_DETAIL_CONCAT(a, b) DVV_DETAIL_CONCAT_INNER(a, b)

// Outside coroutines: DVV_TRY(auto value, parse(text)); returns the error early.
// `declaration` is a declaration (e.g. `auto value`) and cannot be parenthesized.
#define DVV_TRY(declaration, expression)                                                  \
    auto DVV_DETAIL_CONCAT(dvv_try_, __LINE__) = (expression);                            \
    if (!DVV_DETAIL_CONCAT(dvv_try_, __LINE__)) {                                         \
        return std::unexpected(std::move(DVV_DETAIL_CONCAT(dvv_try_, __LINE__)).error()); \
    }                                                                                     \
    declaration = /* NOLINT(bugprone-macro-parentheses) */                                \
        std::move(*DVV_DETAIL_CONCAT(dvv_try_, __LINE__))

// Outside coroutines, for Result<void>: DVV_TRY_VOID(writeFile(path));
#define DVV_TRY_VOID(expression)                                      \
    do {                                                              \
        auto dvv_try_void_ = (expression);                            \
        if (!dvv_try_void_) {                                         \
            return std::unexpected(std::move(dvv_try_void_).error()); \
        }                                                             \
    } while (false)
