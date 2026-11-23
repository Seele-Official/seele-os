#pragma once

// Console output and logging.
//
// print()/println() and the LOG_* macros take a literal format string that is
// validated at compile time: a stray brace, a wrong number of placeholders, an
// unsupported argument type or a specifier that does not fit its argument is
// diagnosed at the call site.
//
// A placeholder is "{}" with an optional spec, e.g. "{:#08x}". The grammar is
// format_spec.h; the short version is that arguments are integers (decimal by
// default, with 'x'/'X'/'b'/'B', an optional '#' prefix and an optional
// zero-padded width), bools (printed as "true"/"false") and strings (const
// char*, char arrays, std::string_view, or anything else convertible to it).
// A spec only makes sense for an integer, so "{:x}" on a string is rejected.
//
// Checking and printing are two passes over the same characters: the consteval
// constructor of format_string only looks, and vprint() re-parses what it is
// handed before writing. A format string therefore has exactly one
// representation -- itself -- and a bad literal still fails at the call site.
//
// Output is asynchronous. A writer formats a whole line under the console
// spinlock and hands it to the attached CharDevice as it fills a staging chunk;
// the device drains its own queue from its interrupt (the UART does), so a log
// line does not wait for the serial line to shift its bytes out and does not
// schedule. This is the property that lets LOG_* and SEELE_ASSERT run from a
// trap handler, from an interrupt handler and from the boot path with
// interrupts disabled.
//
// A device whose queue is full is drained there and then with polled I/O
// rather than dropped: the wait is bounded by the device's queue size and it
// still never schedules, provided the transmitter keeps accepting bytes.
// Backpressure becomes slow output, not lost output.
//
// Input is the one part of the console that is not implemented here: read()
// forwards to the attached CharDevice (char_device.h). The wait for a byte
// belongs to the driver, which owns the receive queue and the interrupt that
// wakes a parked thread, and the console keeps out of it so that no console
// lock is held while a reader sleeps.
//
// Panic output deliberately leaves this mechanism. Once console::enter_panic()
// has been called, every line goes through CharDevice::write_polled() instead:
// no console lock, no device queue and no dependence on an interrupt, so panic()
// can print while it already holds the console lock or the device lock. That is
// also why the recursive-lock edge of the old lock-based console is gone. A
// normal writer that starts while a panic is in progress does not wait for the
// console lock either: if the panicking hart stopped while holding it, the lock
// is never released, and waiting would park the other harts. The write path
// re-reads that latch on every failed lock attempt, which is what turns the
// wait into the lock-free path.
//
// Every normal write holds the console spinlock, so output from different harts
// cannot interleave. A line longer than the console's staging chunk is still
// contiguous in the device queue: the lock is held across the whole line.

#include <array>
#include <cstddef>
#include <cstdint>
#include <source_location>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include "char_device.h"
#include "format_check.h"
#include "format_spec.h"

// One captured argument: a string view, a bool, or a signed or unsigned 32-bit
// or 64-bit integer.
// Every alternative is trivial, so the variant is trivially copyable, trivially
// destructible and can never become valueless.
using format_arg = std::variant<std::string_view, bool, int32_t, uint32_t, int64_t, uint64_t>;

static_assert(std::is_trivially_copyable_v<format_arg>);

namespace format_detail {

template <typename T>
using bare_t = std::remove_cvref_t<T>;

// Everything the console can capture: any integer that fits in 64 bits, or
// anything that can be viewed as a string. This is the constraint on make_arg(),
// so it is also exactly what is_arg_v reports.
template <typename T>
concept arg_source = (std::is_integral_v<bare_t<T>> && sizeof(bare_t<T>) <= sizeof(uint64_t)) ||
                     std::is_convertible_v<bare_t<T>, std::string_view>;

// std::variant's converting constructor cannot disambiguate an `int` between
// int64_t and uint64_t, so the alternative is selected from the argument's
// type: a string stays a string, a bool stays a bool, and an integer narrows to
// the smallest alternative that can hold it.
//
// The argument is taken by reference: the string alternative is a non-owning
// view, so it must point at the caller's object rather than at a copy that dies
// when make_arg() returns.
template <typename T>
    requires arg_source<T>
constexpr format_arg make_arg(const T& value) noexcept {
    using bare = bare_t<T>;
    if constexpr(std::is_convertible_v<bare, std::string_view>) {
        return format_arg{std::in_place_type_t<std::string_view>{}, std::string_view{value}};
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

// An argument is printable exactly when make_arg() accepts it, so the call-site
// check in basic_format_string and the capture in format_arg_store cannot
// disagree about which types are supported.
template <typename T>
constexpr inline bool is_arg_v = requires { make_arg(std::declval<const bare_t<T>&>()); };

// A spec describes how to write a number, so it is only accepted for integers.
// make_arg() widens the integer to one of four widths; bools print as
// "true"/"false" and strings print themselves, so neither takes a spec.
template <typename T>
constexpr inline bool accepts_spec_v =
    std::is_integral_v<bare_t<T>> && !std::is_same_v<bare_t<T>, bool>;

// One flag per argument, in placeholder order: true when a spec is allowed.
template <typename... Args>
consteval std::array<bool, sizeof...(Args)> spec_flags() noexcept {
    return {accepts_spec_v<Args>...};
}

}  // namespace format_detail

// A non-owning view over the arguments captured for one call.
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

// Owns the captured arguments. A temporary store lives until the end of the full
// expression, so it outlives the v* call that consumes it.
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

// The format string accepted by print()/println() and the logging macros.
//
// Construction is consteval, so the literal is checked where the argument list
// is known and a runtime string cannot be converted to this type: use vprint()
// or vprintln() for those. type_identity_t keeps Args non-deduced, matching
// std::format_string.
//
// The object holds nothing but the view: everything the grammar says is checked
// here, and then parsed again from the characters by vprint(). Nothing derived
// from the literal crosses into run time, so a format string has one
// representation: itself.
template <typename... Args>
class basic_format_string {
public:
    template <size_t N>
    consteval basic_format_string(const char (&fmt)[N]) noexcept : m_fmt(fmt, N - 1) {
        static_assert((format_detail::is_arg_v<Args> && ...), "unsupported format argument");
        if(format_check::count<N>(fmt) != sizeof...(Args)) {
            format_error_wrong_number_of_placeholders();
        }

        constexpr std::array<bool, sizeof...(Args)> takes_spec =
            format_detail::spec_flags<Args...>();
        format_check::for_each(fmt, [takes_spec](size_t index, format_spec spec) {
            if(spec_has_spec(spec) && !takes_spec[index]) {
                format_error_format_specifier_needs_integer();
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

// The unchecked layer: the caller guarantees that fmt is well formed and that
// every placeholder has a matching argument. vprint() parses the placeholders
// with the same grammar the compile-time check used.
void vprint(std::string_view fmt, format_args args) noexcept;
void vprintln(std::string_view fmt, format_args args) noexcept;

template <typename... Args>
void print(format_string<Args...> fmt, Args&&... args) noexcept {
    vprint(fmt.get(), make_format_args(args...));
}

template <typename... Args>
void println(format_string<Args...> fmt, Args&&... args) noexcept {
    vprintln(fmt.get(), make_format_args(args...));
}

namespace console {

enum class Level : uint8_t {
    Debug,
    Info,
    Warn,
    Err,
};

// Make `device` both the console's output sink and its input source. Called by
// the driver core when a probed object exposes `CharDevice` through
// `device::dyn_cast`. Flushes any previous sink under the console lock before
// handing over so early-to-driver transition preserves output order.
void attach(CharDevice& device) noexcept;

// Lockless boot-time output attachment for the early platform console
// (platform/early_console.h). Installs an output-only `ConsoleSink` before
// locks, interrupts, or other harts exist; does not enable `console::read()`.
void attach_boot(ConsoleSink& sink) noexcept;

// Whether an output sink (early or full driver) is attached.
[[nodiscard]] bool attached() noexcept;

// Whether an interactive input-capable `CharDevice` is attached. Callers such
// as shell startup check this rather than `attached()`, so an output-only early
// console (such as OpenSBI debug console) never starts a reader that cannot
// block on input.
[[nodiscard]] bool can_read() noexcept;

// Block until the attached device delivers one byte, then return it. Thread
// context only: it requires a current thread, interrupts enabled and no
// spinlock held, and one thread may wait at a time.
//
// This is a forwarder, deliberately: the wait itself belongs to the driver that
// owns the receive queue and the receive interrupt (CharDevice::read() in
// char_device.h). The console takes no lock here, because the driver's wait
// parks the calling thread and a console lock held across it would also block
// the interrupt handler that fills the queue. No device attached is a panic, not
// a silent block: nothing would ever wake the caller.
[[nodiscard]] char read() noexcept;

// Transmit everything the console and the sink have accepted, by polling, and
// return when the device is quiet. Takes the console lock and the device lock,
// so it is thread-context output ordering, not a panic path: use it before a
// handover (shell start) or when an observer must see the output. Never
// schedules. During a panic it degrades to the polled, lock-free path, so it
// cannot wait for a lock the panicking hart may have stopped with.
void flush() noexcept;

// Switch the console to panic output: from the first call on, every line goes
// through CharDevice::write_polled(), which takes no lock and does not depend
// on interrupts, so panic() can print even while it holds the console or the
// device lock. The first call also tells the sink (CharDevice::enter_panic())
// and drains what the asynchronous path has already handed to it, so the panic
// text follows those lines; a line still being formatted on another hart at
// that moment is not covered. Calls after the first are no-ops.
void enter_panic() noexcept;

void
    vlog(Level level, std::source_location source, std::string_view fmt, format_args args) noexcept;

template <typename... Args>
void log(Level level,
         std::source_location source,
         format_string<Args...> fmt,
         Args&&... args) noexcept {
    vlog(level, source, fmt.get(), make_format_args(args...));
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
