#include "util/format.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <variant>

namespace {

// The longest digit string a uint64_t can produce: 64 binary digits. Width
// padding is emitted separately, so the stack buffer never has to hold it.
constexpr size_t MAX_DIGITS = 64;

// Small stack chunk used when emitting repeated padding characters.
constexpr size_t PAD_CHUNK = 32;

// Wraps a FormatSink to count every byte emitted during one vformat_to() call.
class CountingSink {
public:
    explicit CountingSink(FormatSink& sink) noexcept : m_sink(sink) {}

    void write(std::string_view text) noexcept {
        if(text.empty()) {
            return;
        }
        m_sink.write(text);
        m_count += text.size();
    }

    void putc(char c) noexcept {
        m_sink.write(std::string_view{&c, 1});
        ++m_count;
    }

    [[nodiscard]] size_t count() const noexcept {
        return m_count;
    }

private:
    FormatSink& m_sink;
    size_t m_count = 0;
};

// Bounded buffer sink for vformat_to(std::span<char>, ...). Copies at most
// `m_out.size()` bytes without NUL termination and drops any overflow.
class BufferSink final : public FormatSink {
public:
    explicit BufferSink(std::span<char> out) noexcept : m_out(out) {}

    void write(std::string_view text) noexcept override {
        const size_t remaining = m_out.size() - m_written;
        const size_t to_copy = text.size() < remaining ? text.size() : remaining;
        for(size_t i = 0; i < to_copy; ++i) {
            m_out[m_written + i] = text[i];
        }
        m_written += to_copy;
    }

    [[nodiscard]] size_t written() const noexcept {
        return m_written;
    }

private:
    std::span<char> m_out;
    size_t m_written = 0;
};

void write_repeat(CountingSink& out, char fill, size_t count) noexcept {
    if(count == 0) {
        return;
    }
    char block[PAD_CHUNK];
    const size_t block_size = count < PAD_CHUNK ? count : PAD_CHUNK;
    for(size_t i = 0; i < block_size; ++i) {
        block[i] = fill;
    }
    while(count != 0) {
        const size_t step = count < PAD_CHUNK ? count : PAD_CHUNK;
        out.write(std::string_view{block, step});
        count -= step;
    }
}

void write_aligned(std::string_view text, const format_spec& spec, CountingSink& out) noexcept {
    const size_t padding = spec.width > text.size() ? spec.width - text.size() : 0;
    if(padding == 0) {
        out.write(text);
        return;
    }

    // Default alignment for strings, chars, and bools is Left.
    const format_spec::Align align =
        spec.align == format_spec::Align::None ? format_spec::Align::Left : spec.align;
    switch(align) {
        case format_spec::Align::Left:
        case format_spec::Align::None:
            out.write(text);
            write_repeat(out, spec.fill, padding);
            break;
        case format_spec::Align::Right:
            write_repeat(out, spec.fill, padding);
            out.write(text);
            break;
        case format_spec::Align::Center: {
            const size_t left_pad = padding / 2;
            write_repeat(out, spec.fill, left_pad);
            out.write(text);
            write_repeat(out, spec.fill, padding - left_pad);
            break;
        }
    }
}

// Write `magnitude` with the base, sign, prefix, alignment and padding the spec
// asks for. Width counts both sign and base prefix, matching std::format:
// "{:#06x}" on 0x1f writes "0x001f" (6 columns).
void write_integer(uint64_t magnitude,
                   bool negative,
                   const format_spec& spec,
                   bool is_pointer,
                   CountingSink& out) noexcept {
    constexpr char LOWER_DIGITS[] = "0123456789abcdef";
    constexpr char UPPER_DIGITS[] = "0123456789ABCDEF";
    const char* digits = spec_uppercase(spec) ? UPPER_DIGITS : LOWER_DIGITS;
    const unsigned base = spec_base(spec);
    const uint64_t original_magnitude = magnitude;

    char text[MAX_DIGITS];
    size_t pos = MAX_DIGITS;
    do {
        text[--pos] = digits[magnitude % base];
        magnitude /= base;
    } while(magnitude != 0);
    const std::string_view digit_view{text + pos, MAX_DIGITS - pos};

    std::string_view sign_str{};
    if(!is_pointer) {
        if(negative) {
            sign_str = "-";
        } else if(spec.sign == format_spec::Sign::Plus) {
            sign_str = "+";
        } else if(spec.sign == format_spec::Sign::Space) {
            sign_str = " ";
        }
    }

    std::string_view alt_str{};
    if(is_pointer) {
        alt_str = spec_prefix(spec);
    } else if(spec.alternate) {
        if(spec.type != format_spec::Type::Octal || original_magnitude != 0) {
            alt_str = spec_prefix(spec);
        }
    }

    const size_t used = sign_str.size() + alt_str.size() + digit_view.size();
    const size_t padding = spec.width > used ? spec.width - used : 0;

    if(spec.align != format_spec::Align::None) {
        // Explicit alignment overrides '0' zero-padding, matching std::format.
        switch(spec.align) {
            case format_spec::Align::Left:
                out.write(sign_str);
                out.write(alt_str);
                out.write(digit_view);
                write_repeat(out, spec.fill, padding);
                break;
            case format_spec::Align::Right:
                write_repeat(out, spec.fill, padding);
                out.write(sign_str);
                out.write(alt_str);
                out.write(digit_view);
                break;
            case format_spec::Align::Center: {
                const size_t left_pad = padding / 2;
                write_repeat(out, spec.fill, left_pad);
                out.write(sign_str);
                out.write(alt_str);
                out.write(digit_view);
                write_repeat(out, spec.fill, padding - left_pad);
                break;
            }
            case format_spec::Align::None: break;
        }
        return;
    }

    if(!spec.zero_pad) {
        write_repeat(out, ' ', padding);
    }
    out.write(sign_str);
    out.write(alt_str);
    if(spec.zero_pad) {
        write_repeat(out, '0', padding);
    }
    out.write(digit_view);
}

void write_signed(int64_t value, const format_spec& spec, CountingSink& out) noexcept {
    // Negating INT64_MIN overflows, so compute magnitude via unsigned two's complement.
    const uint64_t magnitude =
        value < 0 ? ~static_cast<uint64_t>(value) + 1 : static_cast<uint64_t>(value);
    write_integer(magnitude, value < 0, spec, false, out);
}

// Format one captured argument according to the parsed placeholder spec.
void write_arg(const format_arg& arg, const format_spec& spec, CountingSink& out) noexcept {
    std::visit(
        [&spec, &out](const auto& value) {
            using value_type = std::remove_cvref_t<decltype(value)>;
            if constexpr(std::is_same_v<value_type, std::string_view>) {
                write_aligned(value, spec, out);
            } else if constexpr(std::is_same_v<value_type, bool>) {
                write_aligned(value ? "true" : "false", spec, out);
            } else if constexpr(std::is_same_v<value_type, char>) {
                if(spec_is_integer_type(spec.type)) {
                    if constexpr(std::is_signed_v<char>) {
                        write_signed(static_cast<int64_t>(value), spec, out);
                    } else {
                        write_integer(static_cast<uint64_t>(value), false, spec, false, out);
                    }
                } else {
                    write_aligned(std::string_view{&value, 1}, spec, out);
                }
            } else if constexpr(std::is_same_v<value_type, const void*>) {
                format_spec ptr_spec = spec;
                if(ptr_spec.type != format_spec::Type::UpperPointer) {
                    ptr_spec.type = format_spec::Type::Pointer;
                }
                write_integer(reinterpret_cast<uintptr_t>(value), false, ptr_spec, true, out);
            } else if constexpr(std::is_signed_v<value_type>) {
                write_signed(static_cast<int64_t>(value), spec, out);
            } else {
                write_integer(static_cast<uint64_t>(value), false, spec, false, out);
            }
        },
        arg);
}

}  // namespace

size_t vformat_to(FormatSink& sink, std::string_view fmt, format_args args) noexcept {
    CountingSink out{sink};

    size_t next = 0;
    for(size_t i = 0; i < fmt.size();) {
        if(fmt[i] != '{' && fmt[i] != '}') {
            const size_t start = i;
            while(i < fmt.size() && fmt[i] != '{' && fmt[i] != '}') {
                ++i;
            }
            out.write(std::string_view{fmt.data() + start, i - start});
            continue;
        }

        if(fmt[i] == '}') {
            out.putc('}');
            i += (i + 1 < fmt.size() && fmt[i + 1] == '}') ? 2 : 1;
            continue;
        }

        // Escaped "{{": emit a single literal '{'.
        if(i + 1 < fmt.size() && fmt[i + 1] == '{') {
            out.putc('{');
            i += 2;
            continue;
        }

        // A placeholder runs to the next '}'. Compile-time callers guarantee one
        // exists; an unchecked runtime view may not, in which case the brace is
        // emitted literally instead of swallowing the rest of the string.
        size_t end = i + 1;
        while(end < fmt.size() && fmt[end] != '}') {
            ++end;
        }
        if(end == fmt.size()) {
            out.putc('{');
            ++i;
            continue;
        }

        const std::string_view body{fmt.data() + i + 1, end - i - 1};
        const std::optional<format_spec> spec = parse_placeholder(body);
        if(!spec.has_value()) {
            // Keep text rejected by the grammar so a malformed runtime
            // format string stays readable even when no arguments remain.
            out.write(std::string_view{fmt.data() + i, end - i + 1});
        } else if(next < args.size()) {
            write_arg(args[next], *spec, out);
        }
        ++next;
        i = end + 1;
    }

    return out.count();
}

size_t vformat_to(std::span<char> out, std::string_view fmt, format_args args) noexcept {
    BufferSink sink{out};
    (void)vformat_to(sink, fmt, args);
    return sink.written();
}
