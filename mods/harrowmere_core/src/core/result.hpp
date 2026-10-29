// result.hpp — rule-level failures are return values, never exceptions (§3.4).
//
// Minimal expected-style Result<T, E> for the whole codebase; task 0.4's loader
// API builds directly on it (loadJsonFile(path) -> Result<json, DataError>).
// Storage is std::variant<T, E> — simple, deterministic, no exceptions thrown.
// Accessing the wrong side asserts in debug; it is a programmer error.

#pragma once

#include <cassert>
#include <cstddef>
#include <utility>
#include <variant>

template <class T, class E>
class [[nodiscard]] Result {
public:
    static Result ok(T value) { return Result(std::in_place_index<0>, std::move(value)); }
    static Result err(E error) { return Result(std::in_place_index<1>, std::move(error)); }

    bool isOk() const { return state_.index() == 0; }
    bool isErr() const { return !isOk(); }
    explicit operator bool() const { return isOk(); }

    const T& value() const& { assert(isOk() && "value() on an Err Result"); return std::get<0>(state_); }
    T&       value() &      { assert(isOk() && "value() on an Err Result"); return std::get<0>(state_); }
    T&&      value() &&     { assert(isOk() && "value() on an Err Result"); return std::get<0>(std::move(state_)); }
    const E& error() const  { assert(isErr() && "error() on an Ok Result"); return std::get<1>(state_); }

private:
    template <std::size_t I, class V>
    Result(std::in_place_index_t<I> tag, V&& v) : state_(tag, std::forward<V>(v)) {}

    std::variant<T, E> state_;
};

// void specialization: Result<void, E> carries no value on success.
template <class E>
class [[nodiscard]] Result<void, E> {
public:
    static Result ok() { return Result(); }
    static Result err(E error) { return Result(std::in_place_index<1>, std::move(error)); }

    bool isOk() const { return state_.index() == 0; }
    bool isErr() const { return !isOk(); }
    explicit operator bool() const { return isOk(); }

    const E& error() const { assert(isErr() && "error() on an Ok Result"); return std::get<1>(state_); }

private:
    Result() = default;

    template <class V>
    Result(std::in_place_index_t<1>, V&& v)
        : state_(std::in_place_index<1>, std::forward<V>(v)) {}

    std::variant<std::monostate, E> state_;
};
