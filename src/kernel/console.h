#pragma once

// Console output, compile-time checked formatting, logging, and input dispatch.
//
// - Formatting: print()/println()/LOG_* validate literal format strings at
//   compile time (grammar in format_spec.h; integers, bools, and strings).
//   vprint()/vprintln() re-parse the same string_view at run time.
// - Normal output: serialized per line under the console spinlock and submitted
//   in chunks to the attached ConsoleSink without scheduling (safe in boot,
//   trap, and interrupt contexts; full TX queues drain by polling). Dropped
//   once panic mode is latched so surviving harts do not clutter crash output.
// - Emergency output: print_emergency()/println_emergency()/vprint_emergency()
//   always bypass locks and write straight to ConsoleSink::write_polled(), so
//   panic() and stacktrace::print() cannot deadlock on console or device locks.
// - Input: read() forwards locklessly to the attached CharDevice (char_device.h)
//   so blocking waits never hold the console lock.

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

template <typename T>
constexpr bool is_arg_v = requires { make_arg(std::declval<const bare_t<T>&>()); };

// Format specifiers apply to non-bool integers only.
template <typename T>
constexpr bool accepts_spec_v = std::is_integral_v<bare_t<T>> && !std::is_same_v<bare_t<T>, bool>;

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

// Compile-time checked format string wrapper (use vprint/vprintln for runtime
// strings). type_identity_t keeps Args non-deduced, matching std::format_string.
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

// Strip leading path components ("src/kernel/kmain.cc" -> "kmain.cc") without
// using string_view::substr(), whose range check would pull in abort/terminate.
[[nodiscard]] constexpr std::string_view file_basename(std::string_view file) noexcept {
    const size_t slash = file.find_last_of('/');
    if(slash == std::string_view::npos) {
        return file;
    }
    return std::string_view{file.data() + slash + 1, file.size() - slash - 1};
}

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

// Block until the attached CharDevice delivers one byte. Thread context only
// (current thread, interrupts enabled, no spinlock held, single waiter). Takes
// no console lock; panics if no readable CharDevice is attached.
[[nodiscard]] char read() noexcept;

// Drain all accepted output to the attached ConsoleSink by polling until quiet.
// Takes console + device locks (never schedules). No-op once panic is latched.
void flush() noexcept;

// Latch panic mode and silence the attached sink's interrupts without taking
// locks. Normal console output is dropped afterwards; panic callers print
// through the dedicated emergency functions below.
void enter_panic() noexcept;

// Lock-free emergency output for panic() and stacktrace::print(). Always writes
// directly via ConsoleSink::write_polled() without touching console or device
// locks.
void vprint_emergency(std::string_view fmt, format_args args) noexcept;
void vprintln_emergency(std::string_view fmt, format_args args) noexcept;

template <typename... Args>
void print_emergency(format_string<Args...> fmt, Args&&... args) noexcept {
    vprint_emergency(fmt.get(), make_format_args(args...));
}

template <typename... Args>
void println_emergency(format_string<Args...> fmt, Args&&... args) noexcept {
    vprintln_emergency(fmt.get(), make_format_args(args...));
}

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
