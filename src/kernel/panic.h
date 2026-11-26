#pragma once

// The kernel's last-resort stop, and the debug checks built on it.
//
// panic() prints the message and the call stack that led to it, then parks the
// calling hart. It never returns, and there is nothing to unwind: no caller can
// recover from a panic, so the only job left is to make the failure readable.
//
// The stack trace is best effort. It walks the frame-pointer chain, so frames
// from code built without -fno-omit-frame-pointer (assembly stubs) simply end
// the walk; that is why the output is diagnostics, not a contract.
//
// Implemented in panic.cc, next to the C++ entry points the macros below use.
// The C++ side is namespace debug: a kernel check needs a source position, a
// formatted message and the stack trace, while the C ABI only gets a string.

#include <source_location>
#include <string_view>

#include "util/format.h"

// The C ABI entry point. C++ callers should use debug::panicf(), which also
// records where the panic came from.
extern "C" [[noreturn]] void panic(const char* message);

namespace debug {

// Print "panic: <message>", the caller's source position and the stack trace,
// then park the hart. The message is formatted like println(): the literal is
// checked at the call site and the arguments follow the same grammar.
[[noreturn]] void vpanic(std::source_location location,
                         std::string_view fmt,
                         format_args args) noexcept;

template <typename... Args>
[[noreturn]] void panicf(std::source_location location,
                         format_string<Args...> fmt,
                         Args&&... args) noexcept {
    vpanic(location, fmt.get(), ::make_format_args(args...));
}

// The printer behind SEELE_ASSERT / SEELE_DASSERT. `fmt` is empty when the
// assertion carries no message.
[[noreturn]] void vassert_fail(std::source_location location,
                               const char* condition,
                               std::string_view fmt,
                               format_args args) noexcept;

template <typename... Args>
[[noreturn]] void assert_fail(std::source_location location,
                              const char* condition,
                              format_string<Args...> fmt,
                              Args&&... args) noexcept {
    vassert_fail(location, condition, fmt.get(), ::make_format_args(args...));
}

[[noreturn]] inline void assert_fail(std::source_location location,
                                     const char* condition) noexcept {
    vassert_fail(location, condition, {}, format_args{});
}

}  // namespace debug

// Assert an invariant that must hold in every build. A failed check prints the
// expression, the source position, the optional message and the stack trace,
// then panics.
//
// The condition is evaluated exactly once and is never compiled out: unlike
// <cassert>, this does not follow NDEBUG. A kernel cannot recover from a broken
// invariant anyway, so stopping at the check is cheaper than debugging the
// corruption it causes later. Use SEELE_DASSERT for checks that are too hot or
// too expensive to keep in a release build.
//
// The optional message is a println() format literal:
//
//     SEELE_ASSERT(head != nullptr);
//     SEELE_ASSERT(length <= capacity, "length={} capacity={}", length, capacity);
//
// A condition containing a top-level comma, e.g. std::is_same_v<A, B>, has to
// be parenthesised: SEELE_ASSERT((std::is_same_v<A, B>)). Without the extra
// parentheses the comma splits the macro arguments and the second half is
// parsed as a message.
#define SEELE_ASSERT(condition, ...)                                                               \
    do {                                                                                           \
        if(!(condition)) {                                                                         \
            ::debug::assert_fail(std::source_location::current(),                                  \
                                 #condition __VA_OPT__(, ) __VA_ARGS__);                           \
        }                                                                                          \
    } while(false)

// SEELE_ASSERT for checks that only matter while debugging: compiled out when
// NDEBUG is defined (the Release build), so the condition is not evaluated.
#if defined(NDEBUG)
#define SEELE_DASSERT(condition, ...)                                                              \
    do {                                                                                           \
    } while(false)
#else
#define SEELE_DASSERT(condition, ...) SEELE_ASSERT(condition __VA_OPT__(, ) __VA_ARGS__)
#endif

// Stop with a formatted message, the source position and the stack trace.
#define SEELE_PANIC(...) ::debug::panicf(std::source_location::current(), __VA_ARGS__)

// Mark a point the code must never reach. Unlike std::unreachable() (which is
// undefined behavior or lowers to a bare trap instruction), this panics
// directly with the call site's source location and stack trace.
#define SEELE_UNREACHABLE()                                                                        \
    ::debug::panicf(std::source_location::current(), "unreachable code reached")

// Tell the compiler that `condition` always holds, without checking it. It is
// a proof, not a test: a false condition makes the program's behaviour
// undefined. Reach for SEELE_ASSERT unless the branch shows up in a profile.
#define SEELE_ASSUME(condition) [[assume(condition)]]
