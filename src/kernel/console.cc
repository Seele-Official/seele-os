#include "console.h"

#include <atomic>
#include <cstddef>
#include <optional>
#include <source_location>
#include <string_view>

#include "panic.h"
#include "concurrency/lock_guard.h"
#include "concurrency/spinlock.h"
#include "util/format.h"

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
class Writer final : public FormatSink {
public:
    explicit Writer(bool is_polled) noexcept : m_is_polled(is_polled) {}

    ~Writer() noexcept {
        flush();
    }

    void write(std::string_view text) noexcept override {
        for(char c: text) {
            m_stage[m_count++] = c;
            if(m_count == WRITE_CHUNK) {
                flush();
            }
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

// Format one message into `out`, optionally prepending the log prefix and
// appending a trailing newline.
//
// `out` is the already-chosen output path: the caller either holds the console
// lock (normal write) or has latched the polled panic path.
void vprint_common(std::string_view fmt,
                   format_args args,
                   bool newline,
                   std::optional<log_line> line,
                   Writer& out) noexcept {
    if(line.has_value()) {
        out.write(level_tag(line->level));
        if(line->source.line() != 0) {
            format_to(out,
                      "{}:{}: ",
                      console::file_basename(line->source.file_name()),
                      line->source.line());
        }
    }

    vformat_to(out, fmt, args);

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

void write(std::string_view data) noexcept {
    if(data.empty() || g_panicking.load(std::memory_order_acquire)) {
        return;
    }
    LockGuard guard{g_lock};
    if(ConsoleSink* sink = g_sink.load(std::memory_order_acquire); sink != nullptr) {
        sink->write(data.data(), data.size());
    }
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
