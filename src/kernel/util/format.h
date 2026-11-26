#pragma once

// Freestanding compile-time checked formatting engine.
//
// - Grammar: a subset of C++26 std::format placeholders and brace escapes:
//     '{{' / '}}'  literal '{' / '}'
//     '{' [ ':' spec ] '}'
//     spec  := [ [fill] align ] [ sign ] [ '#' ] [ '0' ] [ width ] [ type ]
//     fill  := any char except '{' and '}' (default ' ')
//     align := '<' (left), '>' (right), '^' (center)
//     sign  := '+' (always sign), '-' (negative only), ' ' (space or '-')
//     '#'   := alternate form ("0x"/"0X"/"0b"/"0B"/"0" for non-zero octal)
//     '0'   := zero-pad after sign/prefix (overridden by explicit align)
//     width := positive decimal integer [1, 255]
//     type  := 'd' decimal, 'x'/'X' hex, 'b'/'B' binary, 'o' octal,
//              'c' char, 's' string/bool, 'p'/'P' pointer
// - Supported argument types: integers up to 64 bits, `char`, `bool`, strings
//   convertible to `std::string_view`, and `void*` / `const void*` /
//   `std::nullptr_t`.
// - Compile-time validation: `format_string<Args...>` validates literal format
//   strings and argument/spec compatibility at the call site via `format_check`
//   (also shared by `format_staticstr()` in util/static_str.h).
// - Type-erased output: `vformat_to()` / `format_to()` write into a caller-
//   provided `FormatSink` (e.g. console's chunked staging writer) or into a
//   bounded `std::span<char>` buffer without heap allocation or exceptions.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

// Parsed placeholder specifier, e.g. "#08x" from "{:#08x}".
// The default-constructed value is "{}": default type, default alignment, no
// sign override, no prefix, no padding.
struct format_spec {
    enum class Type : uint8_t {
        None,
        Decimal,
        Hex,
        UpperHex,
        Binary,
        UpperBinary,
        Octal,
        Char,
        String,
        Pointer,
        UpperPointer,
    };

    enum class Align : uint8_t {
        None,
        Left,
        Right,
        Center,
    };

    enum class Sign : uint8_t {
        None,
        Minus,
        Plus,
        Space,
    };

    Type type = Type::None;
    Align align = Align::None;
    Sign sign = Sign::None;
    char fill = ' ';
    bool alternate = false;
    bool zero_pad = false;
    // Whether a ':' was written at all. "{:d}" formats an integer like "{}",
    // but still constrains its argument type at compile time.
    bool specified = false;
    uint8_t width = 0;
};

[[nodiscard]] constexpr bool spec_is_integer_type(format_spec::Type type) noexcept {
    switch(type) {
        case format_spec::Type::Decimal:
        case format_spec::Type::Hex:
        case format_spec::Type::UpperHex:
        case format_spec::Type::Binary:
        case format_spec::Type::UpperBinary:
        case format_spec::Type::Octal: return true;
        default: return false;
    }
}

// False only for "{}". Used by format_staticstr(), which substitutes whole
// strings and accepts no specifiers.
[[nodiscard]] constexpr bool spec_has_spec(const format_spec& spec) noexcept {
    return spec.specified;
}

[[nodiscard]] constexpr unsigned spec_base(const format_spec& spec) noexcept {
    switch(spec.type) {
        case format_spec::Type::None:
        case format_spec::Type::Decimal: return 10;
        case format_spec::Type::Hex:
        case format_spec::Type::UpperHex:
        case format_spec::Type::Pointer:
        case format_spec::Type::UpperPointer: return 16;
        case format_spec::Type::Binary:
        case format_spec::Type::UpperBinary: return 2;
        case format_spec::Type::Octal: return 8;
        default: return 10;
    }
}

[[nodiscard]] constexpr bool spec_uppercase(const format_spec& spec) noexcept {
    return spec.type == format_spec::Type::UpperHex ||
           spec.type == format_spec::Type::UpperBinary ||
           spec.type == format_spec::Type::UpperPointer;
}

// "" or the base prefix for alternate-form integers and pointers.
[[nodiscard]] constexpr const char* spec_prefix(const format_spec& spec) noexcept {
    switch(spec.type) {
        case format_spec::Type::Hex:
        case format_spec::Type::Pointer: return "0x";
        case format_spec::Type::UpperHex:
        case format_spec::Type::UpperPointer: return "0X";
        case format_spec::Type::Binary: return "0b";
        case format_spec::Type::UpperBinary: return "0B";
        case format_spec::Type::Octal: return "0";
        default: return "";
    }
}

// Parse the text one placeholder carries between its braces: "" for "{}", or
// ':' followed by a spec. Returns std::nullopt when the text is not a valid
// placeholder body.
//
// Shared by compile-time validation (format_check) and runtime formatting
// (vformat_to) so "what compiles" and "what formats" cannot drift apart.
[[nodiscard]] constexpr std::optional<format_spec>
    parse_placeholder(std::string_view body) noexcept {
    constexpr auto parse_align = [](char c) constexpr noexcept -> format_spec::Align {
        switch(c) {
            case '<': return format_spec::Align::Left;
            case '>': return format_spec::Align::Right;
            case '^': return format_spec::Align::Center;
            default: return format_spec::Align::None;
        }
    };

    format_spec spec;
    if(body.empty()) {
        return spec;
    }
    if(body.front() != ':') {
        return std::nullopt;
    }
    body.remove_prefix(1);
    if(body.empty()) {
        // Require a spec after the colon so a placeholder body never means two
        // different things.
        return std::nullopt;
    }
    spec.specified = true;

    size_t i = 0;

    // Optional [fill] align.
    if(i + 1 < body.size() && parse_align(body[i + 1]) != format_spec::Align::None) {
        if(body[i] == '{' || body[i] == '}') {
            return std::nullopt;
        }
        spec.fill = body[i];
        spec.align = parse_align(body[i + 1]);
        i += 2;
    } else if(i < body.size() && parse_align(body[i]) != format_spec::Align::None) {
        spec.align = parse_align(body[i]);
        ++i;
    }

    // Optional sign.
    if(i < body.size()) {
        switch(body[i]) {
            case '+':
                spec.sign = format_spec::Sign::Plus;
                ++i;
                break;
            case '-':
                spec.sign = format_spec::Sign::Minus;
                ++i;
                break;
            case ' ':
                spec.sign = format_spec::Sign::Space;
                ++i;
                break;
            default: break;
        }
    }

    // Optional alternate form ('#').
    if(i < body.size() && body[i] == '#') {
        spec.alternate = true;
        ++i;
    }

    // Optional zero-padding ('0').
    if(i < body.size() && body[i] == '0') {
        spec.zero_pad = true;
        ++i;
    }

    // Optional positive width in [1, 255].
    if(i < body.size() && body[i] >= '1' && body[i] <= '9') {
        unsigned width = 0;
        size_t width_digits = 0;
        while(i < body.size() && body[i] >= '0' && body[i] <= '9') {
            width = width * 10 + static_cast<unsigned>(body[i] - '0');
            if(++width_digits > 3) {
                return std::nullopt;
            }
            ++i;
        }
        if(width > 255) {
            return std::nullopt;
        }
        spec.width = static_cast<uint8_t>(width);
    }

    // Optional presentation type.
    if(i < body.size()) {
        switch(body[i]) {
            case 'd': spec.type = format_spec::Type::Decimal; break;
            case 'x': spec.type = format_spec::Type::Hex; break;
            case 'X': spec.type = format_spec::Type::UpperHex; break;
            case 'b': spec.type = format_spec::Type::Binary; break;
            case 'B': spec.type = format_spec::Type::UpperBinary; break;
            case 'o': spec.type = format_spec::Type::Octal; break;
            case 'c': spec.type = format_spec::Type::Char; break;
            case 's': spec.type = format_spec::Type::String; break;
            case 'p': spec.type = format_spec::Type::Pointer; break;
            case 'P': spec.type = format_spec::Type::UpperPointer; break;
            default: return std::nullopt;
        }
        ++i;
    }

    if(i != body.size()) {
        // Trailing characters, e.g. "{:x8}" or "{:08x0}".
        return std::nullopt;
    }

    // Type-independent validity checks for combinations that are never legal.
    if(spec.alternate &&
       !(spec.type == format_spec::Type::Hex || spec.type == format_spec::Type::UpperHex ||
         spec.type == format_spec::Type::Binary || spec.type == format_spec::Type::UpperBinary ||
         spec.type == format_spec::Type::Octal)) {
        return std::nullopt;
    }
    if(spec.sign != format_spec::Sign::None &&
       !(spec.type == format_spec::Type::None || spec_is_integer_type(spec.type))) {
        return std::nullopt;
    }
    if(spec.zero_pad &&
       !(spec.type == format_spec::Type::None || spec_is_integer_type(spec.type) ||
         spec.type == format_spec::Type::Pointer || spec.type == format_spec::Type::UpperPointer)) {
        return std::nullopt;
    }

    return spec;
}

// Compile-time diagnostic hooks: declared but intentionally undefined so any
// call inside a consteval context fails compilation naming the broken rule.
[[noreturn]] void format_error_unmatched_left_brace();
[[noreturn]] void format_error_unmatched_right_brace();
[[noreturn]] void format_error_unsupported_format_specifier();
[[noreturn]] void format_error_invalid_format_specifier();
[[noreturn]] void format_error_wrong_number_of_placeholders();
[[noreturn]] void format_error_incompatible_format_specifier();

namespace format_check {

// Walk the literal, validating every brace and placeholder body, and hand each
// parsed spec to `on_placeholder(index, spec)`. Returns the placeholder count.
//
// When `AllowEscapes` is true, `{{` and `}}` represent literal braces.
// A '{' inside a placeholder is always an error (the grammar has no nesting).
template <size_t N, bool AllowEscapes = true, typename Fn>
consteval size_t for_each(const char (&fmt)[N], Fn on_placeholder) noexcept {
    size_t count = 0;
    for(size_t i = 0; i < N - 1;) {
        if(fmt[i] == '{') {
            if constexpr(AllowEscapes) {
                if(i + 1 < N - 1 && fmt[i + 1] == '{') {
                    i += 2;
                    continue;
                }
            }
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
            if constexpr(AllowEscapes) {
                if(i + 1 < N - 1 && fmt[i + 1] == '}') {
                    i += 2;
                    continue;
                }
            }
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

// Require exactly `Expected` placeholders, all of them plain "{}", with no
// brace escapes (used by format_staticstr(), whose output length is derived
// purely from N and argument sizes).
template <size_t N, size_t Expected>
consteval void check_plain(const char (&fmt)[N]) noexcept {
    const size_t found = for_each<N, false>(fmt, [](size_t, format_spec spec) {
        if(spec_has_spec(spec)) {
            format_error_unsupported_format_specifier();
        }
    });
    if(found != Expected) {
        format_error_wrong_number_of_placeholders();
    }
}

}  // namespace format_check

// One captured argument: a string view, a char, a bool, a signed or unsigned
// 32-bit or 64-bit integer, or an untyped pointer. Every alternative is trivial,
// so the variant is trivially copyable, trivially destructible and can never
// become valueless.
using format_arg =
    std::variant<std::string_view, char, bool, int32_t, uint32_t, int64_t, uint64_t, const void*>;

static_assert(std::is_trivially_copyable_v<format_arg>);

namespace format_detail {

template <typename T>
using bare_t = std::remove_cvref_t<T>;

// Untyped pointer sources (matches std::format: char*/const char* remain
// strings, while void*/const void*/nullptr_t format as addresses).
template <typename T>
concept pointer_source =
    std::is_same_v<bare_t<T>, void*> || std::is_same_v<bare_t<T>, const void*> ||
    std::is_same_v<bare_t<T>, std::nullptr_t>;

// Everything the formatter can capture: any integer up to 64 bits (including
// char and bool), anything convertible to std::string_view, or an untyped
// pointer.
template <typename T>
concept arg_source = (std::is_integral_v<bare_t<T>> && sizeof(bare_t<T>) <= sizeof(uint64_t)) ||
                     std::is_convertible_v<bare_t<T>, std::string_view> || pointer_source<T>;

// Select the variant alternative explicitly (std::variant cannot disambiguate
// `int` between int64_t and uint64_t). Taken by reference so string_view points
// at the caller's object rather than a temporary copy.
template <typename T>
    requires arg_source<T>
constexpr format_arg make_arg(const T& value) noexcept {
    using bare = bare_t<T>;
    if constexpr(std::is_convertible_v<bare, std::string_view>) {
        return format_arg{std::in_place_type_t<std::string_view>{}, std::string_view{value}};
    } else if constexpr(pointer_source<bare>) {
        return format_arg{std::in_place_type_t<const void*>{}, static_cast<const void*>(value)};
    } else if constexpr(std::is_same_v<bare, char>) {
        return format_arg{std::in_place_type_t<char>{}, value};
    } else if constexpr(std::is_same_v<bare, bool>) {
        return format_arg{std::in_place_type_t<bool>{}, value};
    } else if constexpr(std::is_signed_v<bare>) {
        if constexpr(sizeof(bare) <= sizeof(int32_t)) {
            return format_arg{std::in_place_type_t<int32_t>{}, static_cast<int32_t>(value)};
        } else {
            return format_arg{std::in_place_type_t<int64_t>{}, static_cast<int64_t>(value)};
        }
    } else {
        if constexpr(sizeof(bare) <= sizeof(uint32_t)) {
            return format_arg{std::in_place_type_t<uint32_t>{}, static_cast<uint32_t>(value)};
        } else {
            return format_arg{std::in_place_type_t<uint64_t>{}, static_cast<uint64_t>(value)};
        }
    }
}

template <typename T>
constexpr bool is_arg_v = requires { make_arg(std::declval<const bare_t<T>&>()); };

enum class ArgKind : uint8_t {
    String,
    Char,
    Bool,
    Integer,
    Pointer,
};

template <typename T>
consteval ArgKind classify_arg() noexcept {
    using bare = bare_t<T>;
    if constexpr(std::is_convertible_v<bare, std::string_view>) {
        return ArgKind::String;
    } else if constexpr(pointer_source<bare>) {
        return ArgKind::Pointer;
    } else if constexpr(std::is_same_v<bare, char>) {
        return ArgKind::Char;
    } else if constexpr(std::is_same_v<bare, bool>) {
        return ArgKind::Bool;
    } else {
        return ArgKind::Integer;
    }
}

[[nodiscard]] constexpr bool spec_valid_for(ArgKind kind, const format_spec& spec) noexcept {
    if(!spec.specified) {
        return true;
    }
    switch(kind) {
        case ArgKind::String:
        case ArgKind::Bool:
            return (spec.type == format_spec::Type::None ||
                    spec.type == format_spec::Type::String) &&
                   spec.sign == format_spec::Sign::None && !spec.alternate && !spec.zero_pad;
        case ArgKind::Char:
            if(spec.type == format_spec::Type::None || spec.type == format_spec::Type::Char) {
                return spec.sign == format_spec::Sign::None && !spec.alternate && !spec.zero_pad;
            }
            if(spec_is_integer_type(spec.type)) {
                return !(spec.alternate && spec.type == format_spec::Type::Decimal);
            }
            return false;
        case ArgKind::Integer:
            return (spec.type == format_spec::Type::None || spec_is_integer_type(spec.type)) &&
                   !(spec.alternate && (spec.type == format_spec::Type::None ||
                                        spec.type == format_spec::Type::Decimal));
        case ArgKind::Pointer:
            return (spec.type == format_spec::Type::None ||
                    spec.type == format_spec::Type::Pointer ||
                    spec.type == format_spec::Type::UpperPointer) &&
                   spec.sign == format_spec::Sign::None && !spec.alternate;
    }
    return false;
}

template <typename... Args>
consteval std::array<ArgKind, sizeof...(Args)> arg_kinds() noexcept {
    return {classify_arg<Args>()...};
}

}  // namespace format_detail

// Non-owning view over the arguments captured for one formatting call.
class format_args {
public:
    constexpr format_args() noexcept = default;

    constexpr format_args(const format_arg* data, size_t size) noexcept :
        m_data(data), m_size(size) {}

    [[nodiscard]] constexpr size_t size() const noexcept {
        return m_size;
    }

    [[nodiscard]] constexpr const format_arg& operator[] (size_t index) const noexcept {
        return m_data[index];
    }

private:
    const format_arg* m_data = nullptr;
    size_t m_size = 0;
};

// Owns the captured arguments. A temporary store lives until the end of the
// full expression, outliving the vformat_to()/vprint() call that consumes it.
template <typename... Args>
class format_arg_store {
public:
    constexpr explicit format_arg_store(const Args&... args) noexcept :
        m_args{format_detail::make_arg(args)...} {}

    constexpr operator format_args() const noexcept {
        return format_args{m_args, sizeof...(Args)};
    }

private:
    format_arg m_args[sizeof...(Args) == 0 ? 1 : sizeof...(Args)]{};
};

template <typename... Args>
[[nodiscard]] constexpr format_arg_store<Args...> make_format_args(const Args&... args) noexcept {
    return format_arg_store<Args...>{args...};
}

// Compile-time checked format string wrapper (use vformat_to()/vprint() for
// runtime strings). type_identity_t keeps Args non-deduced, matching
// std::format_string.
template <typename... Args>
class basic_format_string {
public:
    template <size_t N>
    consteval basic_format_string(const char (&fmt)[N]) noexcept : m_fmt(fmt, N - 1) {
        static_assert((format_detail::is_arg_v<Args> && ...), "unsupported format argument");
        if(format_check::count<N>(fmt) != sizeof...(Args)) {
            format_error_wrong_number_of_placeholders();
        }

        constexpr std::array<format_detail::ArgKind, sizeof...(Args)> kinds =
            format_detail::arg_kinds<Args...>();
        format_check::for_each(fmt, [kinds](size_t index, format_spec spec) {
            if(!format_detail::spec_valid_for(kinds[index], spec)) {
                format_error_incompatible_format_specifier();
            }
        });
    }

    [[nodiscard]] constexpr std::string_view get() const noexcept {
        return m_fmt;
    }

private:
    std::string_view m_fmt;
};

template <typename... Args>
using format_string = basic_format_string<std::type_identity_t<Args>...>;

// Type-erased non-owning output destination for vformat_to() / format_to().
//
// Implementations accept contiguous byte chunks in write() and must not depend
// on exceptions or heap allocation. Concrete sinks typically live on the stack
// for the duration of a single formatting call.
class FormatSink {
public:
    virtual void write(std::string_view text) noexcept = 0;

    void putc(char c) noexcept {
        write(std::string_view{&c, 1});
    }

protected:
    FormatSink() noexcept = default;
    ~FormatSink() = default;

    FormatSink(const FormatSink&) = delete;
    FormatSink& operator= (const FormatSink&) = delete;
    FormatSink(FormatSink&&) = delete;
    FormatSink& operator= (FormatSink&&) = delete;
};

// Format `fmt` with `args` into `sink` without compile-time checking of `fmt`.
// Escaped `{{` / `}}` emit literal braces; unclosed `{`, lone `}`, and invalid
// placeholder bodies are emitted literally; extra placeholders beyond
// `args.size()` produce no output. Returns the total number of bytes emitted to
// `sink`.
size_t vformat_to(FormatSink& sink, std::string_view fmt, format_args args) noexcept;

// Compile-time checked formatting into `sink`. Returns the total number of
// bytes emitted to `sink`.
template <typename... Args>
size_t format_to(FormatSink& sink, format_string<Args...> fmt, Args&&... args) noexcept {
    return ::vformat_to(sink, fmt.get(), ::make_format_args(args...));
}

// Format `fmt` with `args` into the bounded buffer `out`. Writes at most
// `out.size()` bytes (silently truncating overflow) and does not append a NUL
// terminator. Returns the number of bytes actually written into `out`.
[[nodiscard]] size_t vformat_to(std::span<char> out,
                                std::string_view fmt,
                                format_args args) noexcept;

// Compile-time checked formatting into the bounded buffer `out`. Writes at most
// `out.size()` bytes without NUL termination and returns the number of bytes
// actually written into `out`.
template <typename... Args>
[[nodiscard]] size_t format_to(std::span<char> out,
                               format_string<Args...> fmt,
                               Args&&... args) noexcept {
    return ::vformat_to(out, fmt.get(), ::make_format_args(args...));
}
