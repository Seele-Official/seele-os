#include "console.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <source_location>
#include <string_view>
#include <type_traits>
#include <variant>

#include "format_spec.h"
#include "panic.h"
#include "concurrency/lock_guard.h"
#include "concurrency/spinlock.h"

namespace {

// Output sink (early console or full driver) and interactive input source (full
// CharDevice only). Output runs under `g_lock`; `read()` loads `g_input`
// without holding `g_lock` so blocking does not stall writers or interrupts.
std::atomic<ConsoleSink*> g_sink{nullptr};
std::atomic<CharDevice*> g_input{nullptr};
Spinlock g_lock{};

// Latched by console::enter_panic() and never cleared. Once set, normal output
// and flush calls return without taking `g_lock` so surviving harts do not
// clutter or contend with the panicking hart's emergency output.
std::atomic<bool> g_panicking{false};

// One staging chunk. Output is formatted into this buffer and handed to the
// sink in one call; the console lock is held across the whole line, so a line
// longer than the chunk is still contiguous in the sink's queue. 64 bytes keeps
// the staging buffer cheap enough to live on any kernel stack, including the
// interrupt stack shared with trap handling.
constexpr size_t WRITE_CHUNK = 64;

// The formatted-byte sink for one vprint()/println() call.
//
// `write`/`write_polled` are the two device paths; `m_is_polled` is fixed at
// construction so that a panic that starts on another hart cannot split this
// line between the queue and the hardware. The destructor submits the last
// partial chunk, which is why every call site only has to write the line.
class Writer {
public:
    explicit Writer(bool is_polled) noexcept : m_is_polled(is_polled) {}

    Writer(const Writer&) = delete;
    Writer& operator= (const Writer&) = delete;

    ~Writer() noexcept {
        flush();
    }

    void putc(char c) noexcept {
        m_stage[m_count++] = c;
        if(m_count == WRITE_CHUNK) {
            flush();
        }
    }

    void append(std::string_view text) noexcept {
        for(char c: text) {
            putc(c);
        }
    }

    void flush() noexcept {
        if(m_count == 0) {
            return;
        }
        if(ConsoleSink* sink = g_sink.load(std::memory_order_acquire); sink != nullptr) {
            if(m_is_polled) {
                sink->write_polled(m_stage, m_count);
            } else {
                sink->write(m_stage, m_count);
            }
        }
        m_count = 0;
    }

private:
    char m_stage[WRITE_CHUNK]{};
    size_t m_count = 0;
    bool m_is_polled = false;
};

// The longest digit string a uint64_t can produce: 64 binary digits. Width
// padding is written separately, so the buffer never has to hold it.
constexpr size_t MAX_DIGITS = 64;

// Write `magnitude` with the base, prefix, sign and padding the spec asks for.
//
// The width counts the prefix and the sign, like std::format: "{:#06x}" on 0x1f
// writes "0x001f" (6 columns), not "0x00001f".
void write_integer(uint64_t magnitude,
                   bool negative,
                   const format_spec& spec,
                   Writer& out) noexcept {
    constexpr char LOWER_DIGITS[] = "0123456789abcdef";
    constexpr char UPPER_DIGITS[] = "0123456789ABCDEF";
    const char* digits = spec_uppercase(spec) ? UPPER_DIGITS : LOWER_DIGITS;
    const unsigned base = spec_base(spec);

    char text[MAX_DIGITS];
    size_t count = 0;
    do {
        text[count++] = digits[magnitude % base];
        magnitude /= base;
    } while(magnitude != 0);

    const char* prefix = "";
    size_t prefix_size = 0;
    if(negative) {
        // A negative value keeps its sign in every base, so the alternate-form
        // prefix would be ambiguous and is dropped.
        prefix = "-";
        prefix_size = 1;
    } else if(spec.alternate) {
        prefix = spec_prefix(spec);
        prefix_size = 2;
    }

    const size_t used = prefix_size + count;
    const size_t padding = spec.width > used ? spec.width - used : 0;

    if(!spec.zero_pad) {
        for(size_t i = 0; i < padding; ++i) {
            out.putc(' ');
        }
    }
    out.append(std::string_view{prefix, prefix_size});
    if(spec.zero_pad) {
        for(size_t i = 0; i < padding; ++i) {
            out.putc('0');
        }
    }
    while(count != 0) {
        out.putc(text[--count]);
    }
}

void write_unsigned(uint64_t value, Writer& out) noexcept {
    write_integer(value, false, format_spec{}, out);
}

// Print one captured argument. make_arg() stores a bool, a string view or one of
// the four integer widths, so the visitor only has to tell those three shapes
// apart; the integers are widened back to the full width and written with the
// spec parsed for this placeholder.
void write_arg(const format_arg& arg, const format_spec& spec, Writer& out) noexcept {
    std::visit(
        [&spec, &out](const auto& value) {
            using value_type = std::remove_cvref_t<decltype(value)>;
            if constexpr(std::is_same_v<value_type, std::string_view>) {
                out.append(value);
            } else if constexpr(std::is_same_v<value_type, bool>) {
                out.append(value ? "true" : "false");
            } else if constexpr(std::is_signed_v<value_type>) {
                const int64_t signed_value = static_cast<int64_t>(value);
                // Negating INT64_MIN overflows, so the magnitude comes from the
                // unsigned two's complement.
                const uint64_t magnitude = signed_value < 0
                                               ? ~static_cast<uint64_t>(signed_value) + 1
                                               : static_cast<uint64_t>(signed_value);
                write_integer(magnitude, signed_value < 0, spec, out);
            } else {
                write_integer(static_cast<uint64_t>(value), false, spec, out);
            }
        },
        arg);
}

[[nodiscard]] constexpr std::string_view level_tag(console::Level level) noexcept {
    switch(level) {
        case console::Level::Debug: return "[DEBUG] ";
        case console::Level::Info: return "[INFO] ";
        case console::Level::Warn: return "[WARN] ";
        case console::Level::Err: return "[ERROR] ";
    }
    return "[UNKNOWN] ";
}

// What a log line adds to its message: the level tag and the position the call
// came from. The two are always known or absent together, so they travel as one
// value; print() and println() pass an empty optional and get no prefix. The
// struct holds nothing but an enum and a source_location, so the optional stays
// trivially copyable and never touches a heap.
struct log_line {
    console::Level level;
    std::source_location source;
};

// Walk a format string and consume one argument per placeholder. This
// deliberately does not diagnose malformed formats: vprint() is the unchecked
// layer and the typed wrappers reject bad literals at compile time. The bounds
// check only keeps a malformed view from reading past its end.
//
// `out` is the already-chosen output path: the caller either holds the console
// lock (normal write) or has latched the polled panic path. `line` is set for
// a log line and empty for plain print()/println() output; when it is set, its
// level tag and source position become the line prefix.
void vprint_common(std::string_view fmt,
                   format_args args,
                   bool newline,
                   std::optional<log_line> line,
                   Writer& out) noexcept {
    if(line.has_value()) {
        out.append(level_tag(line->level));
        if(line->source.line() != 0) {
            out.append(console::file_basename(line->source.file_name()));
            out.putc(':');
            write_unsigned(line->source.line(), out);
            out.append(": ");
        }
    }

    size_t next = 0;
    for(size_t i = 0; i < fmt.size();) {
        if(fmt[i] != '{') {
            out.putc(fmt[i]);
            ++i;
            continue;
        }

        // A placeholder runs to the next '}'. The call-site check made sure the
        // literal has one; a view from an unchecked caller may not, and then the
        // brace is printed literally instead of swallowing the rest of the line.
        size_t end = i + 1;
        while(end < fmt.size() && fmt[end] != '}') {
            ++end;
        }
        if(end == fmt.size()) {
            out.putc('{');
            ++i;
            continue;
        }

        // Parse the body here. The compiler checked the literal already, but a
        // vprint() caller hands over a plain string_view, and this is the only
        // representation of the format that survives to run time. Using the same
        // parser for both is what keeps the check and the printer in agreement.
        const std::optional<format_spec> spec =
            parse_placeholder(std::string_view{fmt.data() + i + 1, end - i - 1});
        if(next < args.size()) {
            if(spec.has_value()) {
                write_arg(args[next], *spec, out);
            } else {
                // A body the grammar rejects: keep the text instead of dropping
                // it, so a broken dynamic format string stays readable.
                out.putc('{');
                out.append(std::string_view{fmt.data() + i + 1, end - i - 1});
                out.putc('}');
            }
        }
        ++next;
        i = end + 1;
    }

    if(newline) {
        out.putc('\n');
    }
}

// Normal output path: drops lines once panic mode is latched; otherwise holds
// `g_lock` for the duration of the line.
void vprint_impl(std::string_view fmt,
                 format_args args,
                 bool newline,
                 std::optional<log_line> line) noexcept {
    if(g_panicking.load(std::memory_order_acquire)) {
        return;
    }
    LockGuard guard{g_lock};
    Writer out{false};
    vprint_common(fmt, args, newline, line, out);
}
}  // namespace

namespace console {

void attach(CharDevice& device) noexcept {
    LockGuard guard{g_lock};
    ConsoleSink* const previous = g_sink.load(std::memory_order_relaxed);
    if(previous != nullptr && previous != static_cast<ConsoleSink*>(&device)) {
        // Finish any buffered output from the previous sink before handing over
        // so early-console-to-driver transition does not reorder output.
        previous->flush();
    }
    g_sink.store(&device, std::memory_order_release);
    g_input.store(&device, std::memory_order_release);
}

void attach_boot(ConsoleSink& sink) noexcept {
    // Lockless: runs on the boot hart before other harts, interrupts, or
    // spinlock misuse paths are ready.
    g_sink.store(&sink, std::memory_order_release);
    g_input.store(nullptr, std::memory_order_release);
}

bool attached() noexcept {
    return g_sink.load(std::memory_order_acquire) != nullptr;
}

bool can_read() noexcept {
    return g_input.load(std::memory_order_acquire) != nullptr;
}

char read() noexcept {
    CharDevice* const device = g_input.load(std::memory_order_acquire);
    if(device == nullptr) {
        SEELE_PANIC("console::read(): no readable console device attached");
    }
    return device->read();
}

void flush() noexcept {
    if(g_panicking.load(std::memory_order_acquire)) {
        return;
    }
    LockGuard guard{g_lock};
    if(ConsoleSink* sink = g_sink.load(std::memory_order_acquire); sink != nullptr) {
        sink->flush();
    }
}

void enter_panic() noexcept {
    g_panicking.store(true, std::memory_order_release);
    if(ConsoleSink* sink = g_sink.load(std::memory_order_acquire); sink != nullptr) {
        sink->enter_panic();
    }
}

void vprint_emergency(std::string_view fmt, format_args args) noexcept {
    Writer out{true};
    vprint_common(fmt, args, false, std::nullopt, out);
}

void vprintln_emergency(std::string_view fmt, format_args args) noexcept {
    Writer out{true};
    vprint_common(fmt, args, true, std::nullopt, out);
}

void vlog(Level level,
          std::source_location source,
          std::string_view fmt,
          format_args args) noexcept {
    vprint_impl(fmt, args, true, log_line{level, source});
}

}  // namespace console

void vprint(std::string_view fmt, format_args args) noexcept {
    vprint_impl(fmt, args, false, std::nullopt);
}

void vprintln(std::string_view fmt, format_args args) noexcept {
    vprint_impl(fmt, args, true, std::nullopt);
}
