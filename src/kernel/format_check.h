#pragma once

#include <cstddef>
#include <optional>
#include <string_view>

#include "format_spec.h"

// Compile-time validation of the console format literal, shared by the console
// formatter and format_staticstr().
//
// This layer only checks: it never parses a literal into data the kernel keeps
// at run time. vprint() re-parses the string it is handed with the same
// parse_placeholder() the checker uses, so "what compiles" and "what prints"
// cannot drift apart.
//
// The error handlers are declared but never defined: a call to one can never be
// part of a constant expression, so the compiler's diagnostic names the rule the
// literal broke. This is the same technique libstdc++ uses for std::format().

[[noreturn]] void format_error_unmatched_left_brace();
[[noreturn]] void format_error_unmatched_right_brace();
[[noreturn]] void format_error_unsupported_format_specifier();
[[noreturn]] void format_error_invalid_format_specifier();
[[noreturn]] void format_error_wrong_number_of_placeholders();
[[noreturn]] void format_error_format_specifier_needs_integer();

namespace format_check {

// Walk the literal, validating every brace and placeholder body, and hand each
// parsed spec to `on_placeholder(index, spec)`. Returns the placeholder count.
//
// A '{' inside a placeholder is an error, not a nested one: the grammar has no
// nesting, so "{a{b}}" must not silently parse as two placeholders.
template <size_t N, typename Fn>
consteval size_t for_each(const char (&fmt)[N], Fn on_placeholder) noexcept {
    size_t count = 0;
    for(size_t i = 0; i < N - 1;) {
        if(fmt[i] == '{') {
            size_t end = i + 1;
            while(end < N - 1 && fmt[end] != '}' && fmt[end] != '{') {
                ++end;
            }
            if(end == N - 1 || fmt[end] != '}') {
                format_error_unmatched_left_brace();
            }

            const std::string_view body{fmt + i + 1, end - (i + 1)};
            const std::optional<format_spec> spec = parse_placeholder(body);
            if(!spec.has_value()) {
                // Keep the two diagnoses apart: "{x}" is not a placeholder the
                // grammar knows at all, "{:08x8}" is one whose spec is broken.
                if(body.empty() || body.front() != ':') {
                    format_error_unsupported_format_specifier();
                }
                format_error_invalid_format_specifier();
            }

            on_placeholder(count, *spec);
            ++count;
            i = end + 1;
        } else if(fmt[i] == '}') {
            format_error_unmatched_right_brace();
        } else {
            ++i;
        }
    }
    return count;
}

template <size_t N>
consteval size_t count(const char (&fmt)[N]) noexcept {
    return for_each(fmt, [](size_t, format_spec) {});
}

// Require exactly `Expected` placeholders, all of them plain "{}".
//
// format_staticstr() substitutes whole strings into an instruction template, so
// a width or a base would have nothing to describe: a spec there is an error.
template <size_t N, size_t Expected>
consteval void check_plain(const char (&fmt)[N]) noexcept {
    const size_t found = for_each(fmt, [](size_t, format_spec spec) {
        if(spec_has_spec(spec)) {
            format_error_unsupported_format_specifier();
        }
    });
    if(found != Expected) {
        format_error_wrong_number_of_placeholders();
    }
}

}  // namespace format_check
