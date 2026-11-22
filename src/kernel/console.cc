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
#include "hart.h"
#include "lock_guard.h"
#include "panic.h"
#include "riscv.h"
#include "spinlock.h"
#include "shed/schedule.h"

namespace {

// The device the console writes through and reads from, or nullptr before a
// driver attaches one. Writers reach it under `g_lock`, which is what serialises
// whole lines; the reader loads it without the lock, because blocking on the
// device while holding a lock would stop the interrupt that fills its queue --
// hence the atomic, which is only about publishing the pointer safely.
std::atomic<CharDevice*> g_device{nullptr};
Spinlock g_lock{};

// Latched by console::enter_panic() and never cleared. A writer checks it before
// it takes the console lock and again once it owns it, then uses the polled path
// for the whole line: the panic path must not depend on a lock or an interrupt,
// and latching per line keeps the chunks of one line on a single path.
//
// The latch is published with release (the exchange is acq_rel) and read with
// acquire: every hart that observes it must also observe the sink state the
// first panic drained, and it must not start a normal line after the panic has
// already taken over the device. The second check is what covers a writer that
// was preempted between the first check and the lock acquisition.
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
class writer {
public:
    explicit writer(bool is_polled) noexcept : m_is_polled(is_polled) {}

    writer(const writer&) = delete;
    writer& operator= (const writer&) = delete;

    ~writer() noexcept {
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
        if(CharDevice* device = g_device.load(std::memory_order_relaxed); device != nullptr) {
            if(m_is_polled) {
                device->write_polled(m_stage, m_count);
            } else {
                device->write(m_stage, m_count);
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
                   writer& out) noexcept {
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

void write_unsigned(uint64_t value, writer& out) noexcept {
    write_integer(value, false, format_spec{}, out);
}

// Print one captured argument. make_arg() stores a bool, a string view or one of
// the four integer widths, so the visitor only has to tell those three shapes
// apart; the integers are widened back to the full width and written with the
// spec parsed for this placeholder.
void write_arg(const format_arg& arg, const format_spec& spec, writer& out) noexcept {
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

// "src/kernel/kmain.cc" becomes "kmain.cc": a log line keeps the interesting part
// of the path no matter how the build spells it. The view is sliced by hand
// instead of with substr(), whose range check would drag an exception path into
// the kernel.
[[nodiscard]] constexpr std::string_view file_basename(std::string_view file) noexcept {
    const size_t slash = file.find_last_of('/');
    if(slash == std::string_view::npos) {
        return file;
    }
    return std::string_view{file.data() + slash + 1, file.size() - slash - 1};
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
                   writer& out) noexcept {
    if(line.has_value()) {
        out.append(level_tag(line->level));
        if(line->source.line() != 0) {
            out.append(file_basename(line->source.file_name()));
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

// The one entry every output function goes through.
//
// A writer normally takes the console lock for the duration of its line. The
// lock is deliberately taken with try_lock in a loop instead of with lock():
// a hart that panics inside the console never releases the lock, and a writer
// that started just before the panic would otherwise spin forever (with
// interrupts off, on the harts that are still running). Re-reading the latch
// on every failed attempt is what turns that wait into the lock-free path.
void vprint_impl(std::string_view fmt,
                 format_args args,
                 bool newline,
                 std::optional<log_line> line) noexcept {
    while(true) {
        if(g_panicking.load(std::memory_order_acquire)) {
            writer out{true};
            vprint_common(fmt, args, newline, line, out);
            return;
        }

        TryLockGuard guard{g_lock};
        if(guard.locked()) {
            // The latch can be set between the check above and this
            // acquisition (the writer may have been preempted in between).
            // Re-read it under the lock: a line committed after a panic must
            // use the polled path too, not the buffered one the panic just
            // drained.
            const bool is_polled = g_panicking.load(std::memory_order_acquire);
            writer out{is_polled};
            vprint_common(fmt, args, newline, line, out);
            return;
        }

        // try_lock reports a lock this hart already owns as busy, so that the
        // panic path does not recurse into the misuse check. Here that would be
        // a writer inside the console critical section: retrying can never
        // succeed, and unlike lock() the misuse check never runs. Say so
        // instead; the panic path is lock-free, so this reports cleanly.
        SEELE_ASSERT(!g_lock.owned_by_current_hart(), "console: recursive output");
    }
}
}  // namespace

namespace console {

void attach(CharDevice& device) noexcept {
    LockGuard guard{g_lock};
    CharDevice* const previous = g_device.load(std::memory_order_relaxed);
    if(previous != nullptr && previous != &device) {
        // The old device may still hold bytes the asynchronous path accepted.
        // Finish them before the new one takes over, so a handover (the early
        // firmware console to the real driver) does not reorder output.
        previous->flush();
    }
    g_device.store(&device, std::memory_order_release);
}

void attach_boot(CharDevice& device) noexcept {
    // No lock and no validation: the early console runs before any other hart
    // or interrupt exists, and a spinlock's misuse checks would call into the
    // panic path that this very device is supposed to carry.
    g_device.store(&device, std::memory_order_release);
}

bool attached() noexcept {
    return g_device.load(std::memory_order_acquire) != nullptr;
}

char read() noexcept {
    // Only the lookup is this layer's: the wait belongs to the device, which
    // owns the receive queue and the interrupt that wakes the reader
    // (CharDevice::read()). No lock is held here, so the device is free to sleep
    // and the interrupt handler is free to take the console lock for output.
    CharDevice* const device = g_device.load(std::memory_order_acquire);
    if(device == nullptr) {
        SEELE_PANIC("console::read(): no console device attached");
    }
    return device->read();
}

void flush() noexcept {
    while(true) {
        if(g_panicking.load(std::memory_order_acquire)) {
            if(CharDevice* device = g_device.load(std::memory_order_acquire); device != nullptr) {
                device->flush_polled();
            }
            return;
        }

        TryLockGuard guard{g_lock};
        if(guard.locked()) {
            // Same second check as vprint_impl(): a panic that latched between
            // the load above and this acquisition must select the polled flush,
            // which does not wait for a device lock the panic path may hold.
            const bool is_polled = g_panicking.load(std::memory_order_acquire);
            if(CharDevice* device = g_device.load(std::memory_order_relaxed); device != nullptr) {
                if(is_polled) {
                    device->flush_polled();
                } else {
                    device->flush();
                }
            }
            return;
        }
        SEELE_ASSERT(!g_lock.owned_by_current_hart(), "console: recursive flush");
    }
}

void enter_panic() noexcept {
    // Tell the device before publishing the console latch. A writer that checked
    // the latch just before it was set then still finds the device's own latch
    // set and takes the polled path instead of queueing bytes behind this drain. The
    // order also keeps the drain honest: it runs after the device has stopped
    // accepting queued output.
    CharDevice* const device = g_device.load(std::memory_order_acquire);
    if(device != nullptr) {
        device->enter_panic();
    }

    // The first panic drains what the asynchronous path already accepted, so the
    // panic text cannot overtake log lines that were handed to the sink before
    // it. Everything after the latch goes through write_polled(), so there is
    // nothing left to drain on later calls.
    if(!g_panicking.exchange(true, std::memory_order_acq_rel)) {
        if(device != nullptr) {
            device->flush_polled();
        }
    }
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
