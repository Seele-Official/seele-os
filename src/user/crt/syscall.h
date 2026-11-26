#pragma once

// Minimal RISC-V U-mode syscall wrappers over `src/uapi/syscall.h`.

#include <cstddef>
#include <cstdint>

#include "uapi/syscall.h"

namespace user::sys {

namespace detail {

inline int64_t ecall1(uint64_t number, uint64_t arg0) noexcept {
    register uint64_t a0 asm("a0") = arg0;
    register uint64_t a7 asm("a7") = number;
    asm volatile("ecall" : "+r"(a0) : "r"(a7) : "memory");
    return static_cast<int64_t>(a0);
}

inline int64_t ecall3(uint64_t number, uint64_t arg0, uint64_t arg1, uint64_t arg2) noexcept {
    register uint64_t a0 asm("a0") = arg0;
    register uint64_t a1 asm("a1") = arg1;
    register uint64_t a2 asm("a2") = arg2;
    register uint64_t a7 asm("a7") = number;
    asm volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
    return static_cast<int64_t>(a0);
}

}  // namespace detail

[[nodiscard]] inline int64_t read(uint64_t fd, void* buf, size_t count) noexcept {
    return detail::ecall3(uapi::sys::READ,
                          fd,
                          reinterpret_cast<uintptr_t>(buf),
                          static_cast<uint64_t>(count));
}

[[nodiscard]] inline int64_t write(uint64_t fd, const void* buf, size_t count) noexcept {
    return detail::ecall3(uapi::sys::WRITE,
                          fd,
                          reinterpret_cast<uintptr_t>(buf),
                          static_cast<uint64_t>(count));
}

[[noreturn]] inline void exit(int64_t status) noexcept {
    (void)detail::ecall1(uapi::sys::EXIT, static_cast<uint64_t>(status));
    while(true) {}
}

[[nodiscard]] inline int64_t shutdown(uint16_t code = 0) noexcept {
    return detail::ecall1(uapi::sys::SHUTDOWN, static_cast<uint64_t>(code));
}

}  // namespace user::sys
