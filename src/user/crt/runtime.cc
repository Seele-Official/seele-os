// Minimal freestanding C/C++ runtime for seele-os user executables.

#include <cstddef>
#include <cstdint>

#include "crt/syscall.h"
#include "uapi/syscall.h"

extern "C" {

    using init_fn = void (*)();
    extern init_fn __init_array_start[];
    extern init_fn __init_array_end[];

    int main();

    [[noreturn]] void __user_start() noexcept {
        for(init_fn* fn = __init_array_start; fn != __init_array_end; ++fn) {
            (*fn)();
        }

        const int status = main();
        user::sys::exit(static_cast<int64_t>(status));
    }

    [[noreturn]] void abort() noexcept {
        static constexpr char MSG[] = "user: abort() called\n";
        (void)user::sys::write(uapi::STDERR_FD, MSG, sizeof(MSG) - 1);
        user::sys::exit(134);
    }

    [[noreturn]] void __cxa_pure_virtual() noexcept {
        static constexpr char MSG[] = "user: pure virtual function called\n";
        (void)user::sys::write(uapi::STDERR_FD, MSG, sizeof(MSG) - 1);
        user::sys::exit(134);
    }

    [[noreturn]] void __cxa_deleted_virtual() noexcept {
        static constexpr char MSG[] = "user: deleted virtual function called\n";
        (void)user::sys::write(uapi::STDERR_FD, MSG, sizeof(MSG) - 1);
        user::sys::exit(134);
    }

}  // extern "C"
