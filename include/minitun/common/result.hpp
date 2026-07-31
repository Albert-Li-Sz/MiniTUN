#pragma once

#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

#include <minitun/common/error.hpp>

namespace minitun::common {

/// C++20-compatible expected-like value used at synchronous and asynchronous
/// API boundaries.
template <typename T> class [[nodiscard]] Result final {
    static_assert(!std::is_reference_v<T>, "Result<T> does not support references");
    static_assert(!std::is_void_v<T>, "Use the Result<void> specialization");
    static_assert(!std::is_same_v<std::remove_cv_t<T>, Error>, "Result<Error> is ambiguous");

  public:
    using value_type = T;
    using error_type = Error;

    Result(const T& value) : storage_(std::in_place_index<0>, value) {}
    Result(T&& value) noexcept(std::is_nothrow_move_constructible_v<T>)
        : storage_(std::in_place_index<0>, std::move(value)) {}
    Result(const Error& error) : storage_(std::in_place_index<1>, error) {}
    Result(Error&& error) noexcept : storage_(std::in_place_index<1>, std::move(error)) {}

    template <typename... Args> [[nodiscard]] static Result success(Args&&... args) {
        return Result(ValueTag{}, std::forward<Args>(args)...);
    }

    [[nodiscard]] static Result failure(Error error) { return Result(std::move(error)); }

    [[nodiscard]] static Result failure(ErrorCode code, std::string message = {}) {
        return Result(Error{code, std::move(message)});
    }

    [[nodiscard]] bool has_value() const noexcept { return storage_.index() == 0; }

    explicit operator bool() const noexcept { return has_value(); }

    T& value() & { return std::get<0>(storage_); }
    const T& value() const& { return std::get<0>(storage_); }
    T&& value() && { return std::get<0>(std::move(storage_)); }
    const T&& value() const&& { return std::get<0>(std::move(storage_)); }

    Error& error() & { return std::get<1>(storage_); }
    const Error& error() const& { return std::get<1>(storage_); }
    Error&& error() && { return std::get<1>(std::move(storage_)); }
    const Error&& error() const&& { return std::get<1>(std::move(storage_)); }

    T& operator*() & { return value(); }
    const T& operator*() const& { return value(); }
    T&& operator*() && { return std::move(*this).value(); }

    T* operator->() { return &value(); }
    const T* operator->() const { return &value(); }

    template <typename U> [[nodiscard]] T value_or(U&& fallback) const& {
        if (has_value()) {
            return value();
        }
        return static_cast<T>(std::forward<U>(fallback));
    }

    template <typename U> [[nodiscard]] T value_or(U&& fallback) && {
        if (has_value()) {
            return std::move(*this).value();
        }
        return static_cast<T>(std::forward<U>(fallback));
    }

  private:
    struct ValueTag final {};

    template <typename... Args>
    explicit Result(ValueTag, Args&&... args)
        : storage_(std::in_place_index<0>, std::forward<Args>(args)...) {}

    std::variant<T, Error> storage_;
};

template <> class [[nodiscard]] Result<void> final {
  public:
    using value_type = void;
    using error_type = Error;

    Result() noexcept = default;
    Result(const Error& error) : error_(error) {}
    Result(Error&& error) noexcept : error_(std::move(error)) {}

    [[nodiscard]] static Result success() noexcept { return Result{}; }

    [[nodiscard]] static Result failure(Error error) { return Result(std::move(error)); }

    [[nodiscard]] static Result failure(ErrorCode code, std::string message = {}) {
        return Result(Error{code, std::move(message)});
    }

    [[nodiscard]] bool has_value() const noexcept { return !error_.has_value(); }
    explicit operator bool() const noexcept { return has_value(); }

    void value() const {
        if (!has_value()) {
            throw std::logic_error("attempted to access the value of a failed Result");
        }
    }

    Error& error() & {
        if (has_value()) {
            throw std::logic_error("attempted to access the error of a successful Result");
        }
        return *error_;
    }

    const Error& error() const& {
        if (has_value()) {
            throw std::logic_error("attempted to access the error of a successful Result");
        }
        return *error_;
    }

    Error&& error() && {
        if (has_value()) {
            throw std::logic_error("attempted to access the error of a successful Result");
        }
        return std::move(*error_);
    }

  private:
    std::optional<Error> error_;
};

} // namespace minitun::common
