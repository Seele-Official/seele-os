#include "panic.h"

#include <cstddef>
#include <source_location>
#include <string_view>

#include "console.h"
#include "power.h"
#include "riscv.h"
#include "stacktrace.h"

#ifndef SEELE_TESTS
#error "SEELE_TESTS is not defined: build the kernel through CMake (see CMakeLists.txt)"
#endif

namespace {

// No interrupt is going to make a panicked hart useful again, so stop taking
// them and wait in the lowest-power state available instead of spinning.
// Clearing the per-source enables matters for more than tidiness: wfi is
// allowed to return immediately while an enabled source is pending, so a hart
// that parked with a pending timer would loop hot instead of sleeping.
[[noreturn]] void park() noexcept {
    riscv::intr_off();
    riscv::csrw(riscv::registers::sie, 0);
#if SEELE_TESTS
    power::poweroff_emergency(1);
#endif
    while(true) {
        riscv::wfi();
    }
}

// The first thing every panic entry does, before any output. Masking interrupts
// keeps the report on this hart: with SIE set, the next timer or external
// interrupt can preempt the panicking code into the trap path, which schedules
// other threads and leaves this hart running them -- the panic would then be
// interleaved with, or never finish, its own report. It also keeps the frame
// chain that stacktrace::print() walks from being swapped out under the walk.
// Latch the console's polled path only afterwards, so no part of the report can
// still take a lock or depend on an interrupt.
void begin_panic() noexcept {
    riscv::intr_off();
    console::enter_panic();
}

void print_position(const char* file, int line, const char* function) noexcept {
    const std::string_view name =
        file != nullptr ? console::file_basename(file) : std::string_view{"(unknown)"};
    console::println_emergency("  at {}:{} in {}",
                               name,
                               line,
                               function != nullptr ? function : "(unknown)");
}

// The one printer behind SEELE_ASSERT / SEELE_DASSERT. The fields arrive split
// apart because std::source_location exposes them that way; an empty `fmt`
// means "no message".
[[noreturn]] void assert_fail_impl(const char* file,
                                   int line,
                                   const char* function,
                                   const char* condition,
                                   std::string_view fmt,
                                   format_args args) noexcept {
    // From here on every line goes through the dedicated lock-free emergency
    // output path straight to hardware polling. That lets panic() report even
    // while already holding the console or UART lock.
    begin_panic();

    console::print_emergency("panic: assertion failed: {}",
                             condition != nullptr ? condition : "(null)");
    if(!fmt.empty()) {
        console::print_emergency(": ");
        console::vprint_emergency(fmt, args);
    }
    console::println_emergency("");
    print_position(file, line, function);
    console::println_emergency("stack trace:");
    stacktrace::print();
    park();
}

}  // namespace

extern "C" [[noreturn]] void panic(const char* message) {
    begin_panic();
    console::println_emergency("panic: {}", message != nullptr ? message : "(null)");
    console::println_emergency("stack trace:");
    stacktrace::print();
    park();
}

void debug::vpanic(std::source_location location, std::string_view fmt, format_args args) noexcept {
    begin_panic();
    console::print_emergency("panic: ");
    console::vprintln_emergency(fmt, args);
    print_position(location.file_name(),
                   static_cast<int>(location.line()),
                   location.function_name());
    console::println_emergency("stack trace:");
    stacktrace::print();
    park();
}

void debug::vassert_fail(std::source_location location,
                         const char* condition,
                         std::string_view fmt,
                         format_args args) noexcept {
    assert_fail_impl(location.file_name(),
                     static_cast<int>(location.line()),
                     location.function_name(),
                     condition,
                     fmt,
                     args);
}
