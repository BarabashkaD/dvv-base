#pragma once

// dvv::MoveOnlyFunction: std::move_only_function where the standard library has it, and
// dvv::detail::MoveOnlyFunction where it does not (Apple's libc++ lacks it).
//
// The detail type supports the signatures R(Args...) and R(Args...) noexcept; there is no const,
// volatile or reference-qualified form. It never uses a small buffer: a non-empty function owns
// one heap block. Calling an empty function asserts in Debug builds, where the standard one is
// undefined.

#include <cassert>
#include <concepts>
#include <cstddef>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>
#include <version>

namespace dvv {
namespace detail {

template <class R, bool Noexcept, class... Args>
class MoveOnlyFunctionBase {
public:
    MoveOnlyFunctionBase() noexcept = default;
    MoveOnlyFunctionBase(std::nullptr_t) noexcept {}  // NOLINT(google-explicit-constructor)

    template <class F>
        requires(!std::derived_from<std::remove_cvref_t<F>, MoveOnlyFunctionBase> &&
                 std::constructible_from<std::decay_t<F>, F> &&
                 std::is_invocable_r_v<R, std::decay_t<F>&, Args...> &&
                 (!Noexcept || std::is_nothrow_invocable_r_v<R, std::decay_t<F>&, Args...>))
    MoveOnlyFunctionBase(
        F&& target)  // NOLINT(google-explicit-constructor,bugprone-forwarding-reference-overload)
        : model_(std::make_unique<Model<std::decay_t<F>>>(std::forward<F>(target))) {}

    MoveOnlyFunctionBase(MoveOnlyFunctionBase&&) noexcept = default;
    MoveOnlyFunctionBase& operator=(MoveOnlyFunctionBase&&) noexcept = default;
    MoveOnlyFunctionBase(const MoveOnlyFunctionBase&) = delete;
    MoveOnlyFunctionBase& operator=(const MoveOnlyFunctionBase&) = delete;
    ~MoveOnlyFunctionBase() = default;

    MoveOnlyFunctionBase& operator=(std::nullptr_t) noexcept {
        model_.reset();
        return *this;
    }

    explicit operator bool() const noexcept { return model_ != nullptr; }

    friend bool operator==(const MoveOnlyFunctionBase& function, std::nullptr_t) noexcept {
        return function.model_ == nullptr;
    }

    R operator()(Args... args) noexcept(Noexcept) {
        assert(model_ != nullptr && "calling an empty MoveOnlyFunction");
        return model_->invoke(std::forward<Args>(args)...);
    }

private:
    class Concept {
    public:
        virtual ~Concept() = default;
        virtual R invoke(Args&&... args) noexcept(Noexcept) = 0;
    };

    template <class F>
    class Model final : public Concept {
    public:
        template <class T>
        // NOLINTNEXTLINE(bugprone-forwarding-reference-overload)
        explicit Model(T&& target) : target_(std::forward<T>(target)) {}

        R invoke(Args&&... args) noexcept(Noexcept) override {
            return std::invoke_r<R>(target_, std::forward<Args>(args)...);
        }

    private:
        F target_;
    };

    std::unique_ptr<Concept> model_;
};

template <class Signature>
class MoveOnlyFunction;

template <class R, class... Args>
class MoveOnlyFunction<R(Args...)> final : public MoveOnlyFunctionBase<R, false, Args...> {
public:
    using MoveOnlyFunctionBase<R, false, Args...>::MoveOnlyFunctionBase;
    MoveOnlyFunction() noexcept = default;
    MoveOnlyFunction(MoveOnlyFunction&&) noexcept = default;
    MoveOnlyFunction& operator=(MoveOnlyFunction&&) noexcept = default;
    ~MoveOnlyFunction() = default;

    MoveOnlyFunction& operator=(std::nullptr_t) noexcept {
        MoveOnlyFunctionBase<R, false, Args...>::operator=(nullptr);
        return *this;
    }

    // Assigning a callable; without this a lambda converts to both candidate move assignments.
    template <class F>
        requires(!std::same_as<std::remove_cvref_t<F>, MoveOnlyFunction> &&
                 std::constructible_from<MoveOnlyFunction, F>)
    MoveOnlyFunction& operator=(F&& target) {
        *this = MoveOnlyFunction(std::forward<F>(target));
        return *this;
    }
};

template <class R, class... Args>
class MoveOnlyFunction<R(Args...) noexcept> final : public MoveOnlyFunctionBase<R, true, Args...> {
public:
    using MoveOnlyFunctionBase<R, true, Args...>::MoveOnlyFunctionBase;
    MoveOnlyFunction() noexcept = default;
    MoveOnlyFunction(MoveOnlyFunction&&) noexcept = default;
    MoveOnlyFunction& operator=(MoveOnlyFunction&&) noexcept = default;
    ~MoveOnlyFunction() = default;

    MoveOnlyFunction& operator=(std::nullptr_t) noexcept {
        MoveOnlyFunctionBase<R, true, Args...>::operator=(nullptr);
        return *this;
    }

    // Assigning a callable; without this a lambda converts to both candidate move assignments.
    template <class F>
        requires(!std::same_as<std::remove_cvref_t<F>, MoveOnlyFunction> &&
                 std::constructible_from<MoveOnlyFunction, F>)
    MoveOnlyFunction& operator=(F&& target) {
        *this = MoveOnlyFunction(std::forward<F>(target));
        return *this;
    }
};

}  // namespace detail

#if defined(__cpp_lib_move_only_function)
template <class Signature>
using MoveOnlyFunction = std::move_only_function<Signature>;
#else
template <class Signature>
using MoveOnlyFunction = detail::MoveOnlyFunction<Signature>;
#endif

}  // namespace dvv
