#pragma once

#include <cstddef>

// Call-stack unwinding for diagnostics.
//
// The kernel is built without .eh_frame (see the -fno-unwind-tables flags in
// CMakeLists.txt), so there is no DWARF CFI to interpret and libgcc's
// _Unwind_Backtrace has nothing to read. Instead the kernel keeps a frame
// pointer chain: -fno-omit-frame-pointer makes GCC open every function with
//
//     addi sp, sp, -N
//     sd   s0, N-16(sp)
//     sd   ra, N-8(sp)
//     addi s0, sp, N
//
// so s0 links each frame to its caller. Unwinding is one pointer chase per
// frame: no tables to relocate, no allocation, no locks -- which is what makes
// it usable from panic().
//
// The cost is a 16-byte save area and a few instructions in every prologue and
// epilogue in the kernel.
namespace stacktrace {

// Maximum number of frames print() reports. A deeper stack is almost always a
// corrupted chain rather than a real call depth this kernel can reach.
constexpr std::size_t MAX_DEPTH = 32;

// Print the call stack of the caller, innermost first:
//
//   #0 0x00000000800012a4
//   #1 0x0000000080000f10
//
// Frame #0 is the return address inside print()'s caller, so print() itself
// never shows up. The addresses are link-time addresses; symbolize them on the
// host with
//
//   riscv64-unknown-elf-addr2line -f -C -i -e build/kernel.elf 0x800012a4
//
// Never allocates and never takes locks: prints directly through
// console::println_emergency(), so it is safe from panic(), traps, and code
// holding the console or device lock.
void print() noexcept;

}  // namespace stacktrace
