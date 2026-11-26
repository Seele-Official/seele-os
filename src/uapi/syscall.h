#pragma once

// Shared kernel <-> user-space ABI definitions: RISC-V syscall numbers,
// standard file descriptors, and negative errno return values.
//
// Calling convention (standard RISC-V Linux ABI):
//   a7      syscall number
//   a0..a5  arguments
//   a0      signed 64-bit return value (>= 0 on success, negative err::* on error)

#include <cstdint>

namespace uapi {

namespace sys {

// Linux RISC-V asm-generic standard syscall numbers.
inline constexpr uint64_t READ = 63;
inline constexpr uint64_t WRITE = 64;
inline constexpr uint64_t EXIT = 93;

// seele-os custom extension range (>= 1000, outside Linux asm-generic).
inline constexpr uint64_t SHUTDOWN = 1000;

}  // namespace sys

inline constexpr uint64_t STDIN_FD = 0;
inline constexpr uint64_t STDOUT_FD = 1;
inline constexpr uint64_t STDERR_FD = 2;

namespace err {

inline constexpr int64_t BADF = -9;    // EBADF: invalid file descriptor
inline constexpr int64_t FAULT = -14;  // EFAULT: invalid or inaccessible user buffer
inline constexpr int64_t NODEV = -19;  // ENODEV: backing device not attached
inline constexpr int64_t INVAL = -22;  // EINVAL: invalid argument
inline constexpr int64_t NOSYS = -38;  // ENOSYS: unimplemented syscall number

}  // namespace err

}  // namespace uapi
