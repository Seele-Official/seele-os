// Minimal C library: process control.
//
// No kernel source calls abort() directly. It exists because libstdc++'s
// failure paths reference it even with -fno-exceptions: std::get() on the
// wrong variant alternative, value() on an empty optional/expected, and -- in
// Debug builds -- every __glibcxx_assert() in the headers (the freestanding
// headers use the non-verbose assertion path, which lowers to abort()).
//
// abort() is the last resort of the C++ code, and panic() is the kernel's, so
// the two are wired together: a libstdc++ precondition failure prints a message
// and a stack trace instead of parking the hart silently. That inverts the
// usual layering (a C library calling into the kernel), but abort() cannot
// return anyway and the kernel is the only thing in the image that can report
// why the hart stopped.

extern "C" {
    [[noreturn]] void panic(const char* message);

    [[noreturn]] void abort(void) {
        panic("abort() called");
    }
}
