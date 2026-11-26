#pragma once

// Synchronous kernel console output: logging, formatted printing, and panic
// emergency diagnostics.
//
// - Formatting: print()/println()/LOG_* validate literal format strings at
//   compile time via util/format.h; vprint()/vprintln() parse runtime
//   string_views with the same grammar via vformat_to().
// - Log / normal output (Path 2): serialized per call under the console
//   spinlock and transmitted synchronously in chunks via
//   ConsoleSink::write_sync() without scheduling (safe in early boot, normal
//   threads, traps, and non-reentrant interrupts). Dropped once panic mode is
//   latched so surviving harts do not clutter crash output.
// - Emergency output (Path 1): print_emergency()/println_emergency()/
//   vprint_emergency() always bypass locks and write straight to
//   ConsoleSink::write_emergency(), so panic() and stacktrace::print() cannot
//   deadlock on console or device locks.
// - Interactive syscall I/O (Path 3) is separate and runs directly against the
//   schedulable `CharDevice` interface (char_device.h).

#include <cstddef>
#include <cstdint>
#include <source_location>
#include <string_view>

#include "console_sink.h"
#include "util/format.h"

// The unchecked layer: the caller guarantees that fmt is well formed and that
// every placeholder has a matching argument. vprint() formats through
// vformat_to() with the same grammar the compile-time check used.
void vprint(std::string_view fmt, format_args args) noexcept;
void vprintln(std::string_view fmt, format_args args) noexcept;

template <typename... Args>
void print(format_string<Args...> fmt, Args&&... args) noexcept {
    ::vprint(fmt.get(), ::make_format_args(args...));
}

template <typename... Args>
void println(format_string<Args...> fmt, Args&&... args) noexcept {
    ::vprintln(fmt.get(), ::make_format_args(args...));
}

namespace console {

enum class Level : uint8_t {
    Debug,
    Info,
    Warn,
    Err,
};

// Strip leading path components ("src/kernel/kmain.cc" -> "kmain.cc") without
// using string_view::substr(), whose range check would pull in abort/terminate.
[[nodiscard]] constexpr std::string_view file_basename(std::string_view file) noexcept {
    const size_t slash = file.find_last_of('/');
    if(slash == std::string_view::npos) {
        return file;
    }
    return std::string_view{file.data() + slash + 1, file.size() - slash - 1};
}

// Install `sink` as the active synchronous console output backend. Called by
// the driver core when a probed object exposes `ConsoleSink` through
// `device::dyn_cast`. Flushes any previous sink under the console lock before
// handing over so early-to-driver transition preserves output order.
void attach(ConsoleSink& sink) noexcept;

// Lockless boot-time output attachment for the early platform console
// (platform/early_console.h). Installs a polled `ConsoleSink` before locks,
// interrupts, or other harts exist.
void attach_boot(ConsoleSink& sink) noexcept;

// Whether an output sink (early or full driver) is attached.
[[nodiscard]] bool attached() noexcept;

// Drain all pending output on the attached ConsoleSink (including any buffered
// async CharDevice TX on a shared UART) and wait until hardware is quiet.
// Takes console + device locks (never schedules). No-op once panic is latched.
void flush() noexcept;

// Latch panic mode and silence the attached sink's interrupts without taking
// locks. Normal console output is dropped afterwards; panic callers print
// through the dedicated emergency functions below.
void enter_panic() noexcept;

// Lock-free emergency output for panic() and stacktrace::print(). Always writes
// directly via ConsoleSink::write_emergency() without touching console or
// device locks.
void vprint_emergency(std::string_view fmt, format_args args) noexcept;
void vprintln_emergency(std::string_view fmt, format_args args) noexcept;

template <typename... Args>
void print_emergency(format_string<Args...> fmt, Args&&... args) noexcept {
    vprint_emergency(fmt.get(), ::make_format_args(args...));
}

template <typename... Args>
void println_emergency(format_string<Args...> fmt, Args&&... args) noexcept {
    vprintln_emergency(fmt.get(), ::make_format_args(args...));
}

void
    vlog(Level level, std::source_location source, std::string_view fmt, format_args args) noexcept;

template <typename... Args>
void log(Level level,
         std::source_location source,
         format_string<Args...> fmt,
         Args&&... args) noexcept {
    vlog(level, source, fmt.get(), ::make_format_args(args...));
}

}  // namespace console

#define LOG_DEBUG(...)                                                                             \
    ::console::log(::console::Level::Debug, std::source_location::current(), __VA_ARGS__)
#define LOG_INFO(...)                                                                              \
    ::console::log(::console::Level::Info, std::source_location::current(), __VA_ARGS__)
#define LOG_WARN(...)                                                                              \
    ::console::log(::console::Level::Warn, std::source_location::current(), __VA_ARGS__)
#define LOG_ERR(...)                                                                               \
    ::console::log(::console::Level::Err, std::source_location::current(), __VA_ARGS__)
