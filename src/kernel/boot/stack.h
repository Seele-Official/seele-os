#pragma once

// Per-hart boot stack layout, shared by the assembly entry stub and C++.
//
// _entry (start.S) is the only code that runs before a stack exists. It picks
// the stack from the hartid before touching sp:
//
//   - hart 0 is the boot hart and gets the large stack (SEELE_BOOT_STACK_SIZE),
//     because it runs the whole boot path: cxx_init, FDT parsing, the early
//     console and driver probing;
//   - every other hart gets a small stack (SEELE_SECONDARY_STACK_SIZE) and a
//     dense cpuid, handed out by an atomic arrival-order counter.
//
// The stacks are members of one C++ object (boot_stacks in start.cc), not
// separate objects, so the compiler and the assembler cannot disagree about
// their offsets. Every stack is page aligned and a page multiple, which is what
// lets vm::k::init() map the pool in the identity map as one run and lets each
// hart later free its own slice page by page (kmain.cc: free_boot_stack())
// without a page being shared with its neighbour. Thread stacks are separate:
// each thread has its own slot in its process's kernel stack window. Kernel
// threads on a hart belong to that hart's system process.
//
// The pool covers every possible cpuid, so the number of slots is exactly
// MAX_HARTS in hart.h -- there is no separate "how many boot stacks" knob. One
// number sizes g_cpus[] and the pool, so they cannot disagree; start.cc
// static_asserts the equality as a safety net.
//
// The three sizes below are build parameters passed as -D by CMakeLists.txt
// ("Kernel build parameters"), not source constants: the pool is this kernel's
// biggest static object, so a board port or a smaller build tunes it on the
// configure command line. They are guarded here rather than defaulted, because a
// build that forgot a -D would otherwise lay the pool out with numbers the
// assembly entry stub and boot::stack_offset() do not share.
#ifndef SEELE_BOOT_STACK_SIZE
#error "SEELE_BOOT_STACK_SIZE is not defined: build through CMake (see CMakeLists.txt)"
#endif
#ifndef SEELE_SECONDARY_STACK_SIZE
#error "SEELE_SECONDARY_STACK_SIZE is not defined: build through CMake (see CMakeLists.txt)"
#endif
#ifndef SEELE_MAX_HARTS
#error "SEELE_MAX_HARTS is not defined: build through CMake (see CMakeLists.txt)"
#endif

#ifdef __cplusplus
#include <cstddef>

#include "hart.h"

namespace boot {

constexpr std::size_t STACK_COUNT = MAX_HARTS;
constexpr std::size_t BOOT_STACK_SIZE = SEELE_BOOT_STACK_SIZE;
constexpr std::size_t SECONDARY_STACK_SIZE = SEELE_SECONDARY_STACK_SIZE;
constexpr std::size_t POOL_SIZE = BOOT_STACK_SIZE + (STACK_COUNT - 1) * SECONDARY_STACK_SIZE;

// Byte offset of stack `index` from the base of boot_stacks, and its size.
// Index 0 is the boot hart's stack; 1..STACK_COUNT-1 are the secondary stacks
// in cpuid order. These definitions mirror the boot_stack_pool object in start.cc;
// the static_asserts there keep the two in sync.
constexpr std::size_t stack_offset(std::size_t index) noexcept {
    if(index == 0) {
        return 0;
    }
    return BOOT_STACK_SIZE + (index - 1) * SECONDARY_STACK_SIZE;
}

constexpr std::size_t stack_size(std::size_t index) noexcept {
    return index == 0 ? BOOT_STACK_SIZE : SECONDARY_STACK_SIZE;
}

}  // namespace boot
#endif
