#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

// Placeholder specifiers, e.g. the "#08x" in "{:#08x}". The grammar is a strict
// subset of std::format's:
//
//   '{' [ ':' spec ] '}'
//
//   spec := [ '#' ] [ '0' ] [ width ] [ type ]
//
//   '#'     alternate form: "0x"/"0X"/"0b"/"0B" in front of the digits
//   '0'     pad with zeros instead of spaces; the width counts the prefix
//   width   one to three decimal digits, at most 255
//   type    'd' decimal (the default), 'x'/'X' hexadecimal, 'b'/'B' binary
//
// Everything else -- fill, alignment, sign, precision, locale, field numbers --
// is deliberately absent: the kernel prints diagnostics, not tables.
//
// The default-constructed value is "{}": decimal, no prefix, no padding.

struct format_spec {
    enum class Type : uint8_t {
        Decimal,
        Hex,
        UpperHex,
        Binary,
        UpperBinary,
    };

    Type type = Type::Decimal;
    bool alternate = false;
    bool zero_pad = false;
    // Whether a ':' was written at all. "{:d}" writes exactly like "{}", but it
    // still claims its argument is a number, which is what the call-site check
    // cares about.
    bool specified = false;
    uint8_t width = 0;
};

// False only for "{}". Used to reject a spec on a non-integer argument, and by
// format_staticstr(), which substitutes whole strings and takes no spec.
[[nodiscard]] constexpr bool spec_has_spec(const format_spec& spec) noexcept {
    return spec.specified;
}

[[nodiscard]] constexpr unsigned spec_base(const format_spec& spec) noexcept {
    switch(spec.type) {
        case format_spec::Type::Decimal: return 10;
        case format_spec::Type::Hex:
        case format_spec::Type::UpperHex: return 16;
        case format_spec::Type::Binary:
        case format_spec::Type::UpperBinary: return 2;
    }
    return 10;
}

[[nodiscard]] constexpr bool spec_uppercase(const format_spec& spec) noexcept {
    return spec.type == format_spec::Type::UpperHex || spec.type == format_spec::Type::UpperBinary;
}

// "" or the two characters the alternate form puts in front of the digits.
[[nodiscard]] constexpr const char* spec_prefix(const format_spec& spec) noexcept {
    switch(spec.type) {
        case format_spec::Type::Decimal: return "";
        case format_spec::Type::Hex: return "0x";
        case format_spec::Type::UpperHex: return "0X";
        case format_spec::Type::Binary: return "0b";
        case format_spec::Type::UpperBinary: return "0B";
    }
    return "";
}

// Parse the text one placeholder carries between its braces: "" for "{}", or
// ':' followed by a spec. nullopt means the text is not a valid placeholder
// body.
//
// This is the only implementation of the grammar. format_check walks a literal
// at compile time and turns nullopt into a diagnostic; vprint_common walks the
// string_view it was handed at run time and prints a rejected placeholder
// literally. Sharing the parser is what keeps "what compiles" and "what prints"
// from drifting apart.
[[nodiscard]] constexpr std::optional<format_spec>
    parse_placeholder(std::string_view body) noexcept {
    format_spec spec;
    if(body.empty()) {
        return spec;
    }
    if(body.front() != ':') {
        return std::nullopt;
    }
    body.remove_prefix(1);
    if(body.empty()) {
        // "{:}" is legal std::format for "default spec"; the kernel's subset
        // requires a spec after the colon, so that a placeholder body never
        // means two different things.
        return std::nullopt;
    }
    spec.specified = true;

    size_t i = 0;
    if(body[i] == '#') {
        spec.alternate = true;
        ++i;
    }
    if(i < body.size() && body[i] == '0') {
        spec.zero_pad = true;
        ++i;
    }

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

    if(i < body.size()) {
        switch(body[i]) {
            case 'd': spec.type = format_spec::Type::Decimal; break;
            case 'x': spec.type = format_spec::Type::Hex; break;
            case 'X': spec.type = format_spec::Type::UpperHex; break;
            case 'b': spec.type = format_spec::Type::Binary; break;
            case 'B': spec.type = format_spec::Type::UpperBinary; break;
            default: return std::nullopt;
        }
        ++i;
    }
    if(i != body.size()) {
        // Trailing characters, e.g. "{:x8}" or "{:08x0}".
        return std::nullopt;
    }
    if(spec.alternate && spec.type == format_spec::Type::Decimal) {
        // '#' only means something for a base that has a prefix.
        return std::nullopt;
    }
    return spec;
}
