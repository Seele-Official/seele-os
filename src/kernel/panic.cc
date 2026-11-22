#include "panic.h"

#include <cstddef>
#include <source_location>
#include <string_view>

#include "console.h"
#include "riscv.h"
#include "stacktrace.h"

namespace {

// "src/kernel/kmain.cc" becomes "kmain.cc", the same rule console.cc uses for
// its LOG_* prefix: the file name identifies the call site and the directory
// the build happened to use is noise in a panic message. The view is sliced by
// hand because string_view::substr() carries a range-check path the kernel does
// not need.
[[nodiscard]] std::string_view file_basename(std::string_view file) noexcept {
    const size_t slash = file.find_last_of('/');
    if(slash == std::string_view::npos) {
        return file;
    }
    return std::string_view{file.data() + slash + 1, file.size() - slash - 1};
}

// No interrupt is going to make a panicked hart useful again, so stop taking
// them and wait in the lowest-power state available instead of spinning.
// Clearing the per-source enables matters for more than tidiness: wfi is
// allowed to return immediately while an enabled source is pending, so a hart
// that parked with a pending timer would loop hot instead of sleeping.
[[noreturn]] void park() noexcept {
    riscv::intr_off();
    riscv::csrw(riscv::registers::sie, 0);
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
        file != nullptr ? file_basename(file) : std::string_view{"(unknown)"};
    println("  at {}:{} in {}", name, line, function != nullptr ? function : "(unknown)");
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
    // From here on every line goes straight to the hardware by polling. That is
    // what lets panic() print while it already holds the console lock or the
    // UART lock, and why it never needs the scheduler or an interrupt.
    begin_panic();

    print("panic: assertion failed: {}", condition != nullptr ? condition : "(null)");
    if(!fmt.empty()) {
        print(": ");
        vprint(fmt, args);
    }
    println("");
    print_position(file, line, function);
    println("stack trace:");
    stacktrace::print();
    park();
}

}  // namespace

extern "C" [[noreturn]] void panic(const char* message) {
    begin_panic();
    println("panic: {}", message != nullptr ? message : "(null)");
    println("stack trace:");
    stacktrace::print();
    park();
}

void debug::vpanic(std::source_location location, std::string_view fmt, format_args args) noexcept {
    begin_panic();
    print("panic: ");
    vprintln(fmt, args);
    print_position(location.file_name(),
                   static_cast<int>(location.line()),
                   location.function_name());
    println("stack trace:");
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
