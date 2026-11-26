// Host-side panic stubs for hardware-independent kernel headers (e.g. `util/list.h`)
// tested under GoogleTest. Prints the diagnostic to stderr and aborts so
// `EXPECT_DEATH` can match the failure message in a forked child process.

#include <cstdio>
#include <cstdlib>
#include <print>
#include <source_location>
#include <string_view>

#include "panic.h"

extern "C" [[noreturn]] void panic(const char* message) {
    std::println(stderr, "panic: {}", message != nullptr ? message : "(null)");
    std::abort();
}

void debug::vpanic(std::source_location location, std::string_view fmt, format_args) noexcept {
    std::println(stderr, "panic at {}:{}: {}", location.file_name(), location.line(), fmt);
    std::abort();
}

void debug::vassert_fail(std::source_location location,
                         const char* condition,
                         std::string_view fmt,
                         format_args) noexcept {
    std::println(stderr,
                 "assertion failed at {}:{}: {} ({})",
                 location.file_name(),
                 location.line(),
                 condition != nullptr ? condition : "(null)",
                 fmt);
    std::abort();
}
