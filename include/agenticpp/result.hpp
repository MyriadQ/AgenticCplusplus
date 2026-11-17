#pragma once

#include <optional>
#include <string>
#include <utility>
#include <variant>

#include "agenticpp/types.hpp"

namespace agenticpp {

// Value-or-error. Expected failures (I/O, parse, HTTP) travel here; exceptions are
// reserved for programmer errors such as calling value() on an error result.
template <class T>
class Result {
public:
    Result() = delete;

    static Result ok(T value) { return Result(std::move(value)); }

    static Result err(Error error) { return Result(std::move(error)); }

    static Result err(int code, std::string message) {
        return Result(Error{code, std::move(message)});
    }

    [[nodiscard]] bool has_value() const noexcept { return std::holds_alternative<T>(storage_); }

    explicit operator bool() const noexcept { return has_value(); }

    T& value() & { return std::get<T>(storage_); }
    const T& value() const& { return std::get<T>(storage_); }
    T&& value() && { return std::get<T>(std::move(storage_)); }

    Error& error() & { return std::get<Error>(storage_); }
    const Error& error() const& { return std::get<Error>(storage_); }
    Error&& error() && { return std::get<Error>(std::move(storage_)); }

    T& operator*() & { return value(); }
    const T& operator*() const& { return value(); }
    T&& operator*() && { return std::move(*this).value(); }

    T* operator->() { return &value(); }
    const T* operator->() const { return &value(); }

private:
    explicit Result(T value) : storage_(std::in_place_index<0>, std::move(value)) {}
    explicit Result(Error error) : storage_(std::in_place_index<1>, std::move(error)) {}

    std::variant<T, Error> storage_;
};

template <>
class Result<void> {
public:
    static Result ok() noexcept { return Result{}; }

    static Result err(Error error) { return Result(std::move(error)); }

    static Result err(int code, std::string message) {
        return Result(Error{code, std::move(message)});
    }

    [[nodiscard]] bool has_value() const noexcept { return !error_.has_value(); }

    explicit operator bool() const noexcept { return has_value(); }

    const Error& error() const { return error_.value(); }

private:
    Result() noexcept = default;
    explicit Result(Error error) : error_(std::move(error)) {}

    std::optional<Error> error_;
};

}  // namespace agenticpp
