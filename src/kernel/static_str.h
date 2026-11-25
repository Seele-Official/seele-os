#pragma once

#include <cstddef>
#include <string_view>

#include "format_check.h"

// Compile-time string used to build CSR instruction templates.
//
// N counts the terminating '\0', so size() is N - 1. The type is structural
// (all members public, nothing but a char array), which is what lets GCC
// accept a constexpr StaticString as the template of an extended `asm`
// statement: the operand must be a string literal or a constexpr object with
// `size` and `data` members.
template <size_t N>
struct StaticString {
    static_assert(N > 0, "StaticString must be non-empty");
    char m_data[N]{};

    // Zero-initialised: the result of a concatenation must stay NUL
    // terminated even if a caller forgets to write the terminator.
    constexpr StaticString() noexcept = default;

    consteval StaticString(const char (&str)[N]) noexcept {
        for(size_t i = 0; i < N; ++i) {
            m_data[i] = str[i];
        }
    }

    constexpr const char* data() const noexcept {
        return m_data;
    }

    constexpr char* data() noexcept {
        return m_data;
    }

    constexpr size_t size() const noexcept {
        return N - 1;
    }

    constexpr char& operator[] (size_t i) noexcept {
        return m_data[i];
    }

    constexpr const char& operator[] (size_t i) const noexcept {
        return m_data[i];
    }

    constexpr operator std::string_view() const noexcept {
        return std::string_view(m_data, N - 1);
    }

    template <size_t M>
    consteval StaticString<N + M - 1> operator+ (const StaticString<M>& other) const noexcept {
        StaticString<N + M - 1> result;
        for(size_t i = 0; i < N - 1; ++i) {
            result[i] = m_data[i];
        }
        for(size_t i = 0; i < M; ++i) {
            result[N - 1 + i] = other.m_data[i];
        }
        return result;
    }

    template <size_t M>
    consteval StaticString<N + M - 1> operator+ (const char (&str)[M]) const noexcept {
        return *this + StaticString<M>{str};
    }
};

template <size_t N, size_t... Ns>
constexpr size_t formatted_capacity =
    (N > 2 * sizeof...(Ns) ? N - 2 * sizeof...(Ns) : 1) + (0 + ... + (Ns - 1));

// Compile-time formatting: each "{}" is replaced by the next argument string,
// left to right. Like std::format, the literal is parsed where the argument
// list is known, so a bad format string is diagnosed at the call site instead
// of failing later. Because the result length is part of its type, the whole
// formatting runs inside this consteval function and the returned StaticString
// is exactly sized.
template <size_t N, size_t... Ns>
consteval auto format_staticstr(const char (&fmt)[N], const StaticString<Ns>&... args) noexcept {
    format_check::check_plain<N, sizeof...(Ns)>(fmt);

    StaticString<formatted_capacity<N, Ns...>> result;
    const std::string_view values[] = {std::string_view(args)..., std::string_view{}};
    size_t argument = 0;
    size_t out = 0;
    for(size_t i = 0; i < N - 1;) {
        if(fmt[i] == '{') {
            for(char value: values[argument]) {
                result[out++] = value;
            }
            ++argument;
            i += 2;
        } else {
            result[out++] = fmt[i];
            ++i;
        }
    }

    return result;
}
