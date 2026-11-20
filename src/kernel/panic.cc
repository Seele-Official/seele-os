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

// No interrupt is going to make a panicked hart useful again, so wait in the
// lowest-power state available instead of spinning.
[[noreturn]] void park() noexcept {
    while(true) {
        riscv::wfi();
    }
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
    // Latch the console's panic path first: from here on every line goes
    // straight to the hardware by polling. That is what lets panic() print
    // while it already holds the console lock or the UART lock, and why it never
    // needs the scheduler or an interrupt.
    console::enter_panic();

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
    console::enter_panic();
    println("panic: {}", message != nullptr ? message : "(null)");
    println("stack trace:");
    stacktrace::print();
    park();
}

void debug::vpanic(std::source_location location, std::string_view fmt, format_args args) noexcept {
    console::enter_panic();
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
