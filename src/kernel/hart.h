#pragma once

#include <cstddef>
#include <cstdint>

#include "list.h"
#include "trap.h"
#include "concurrency/spinlock.h"
#include "proc/process.h"

// Maximum number of harts the kernel can describe.
//
// A build parameter, not a source constant: CMake passes it as -DSEELE_MAX_HARTS
// (see CMakeLists.txt, "Kernel build parameters") and the guard below makes a
// build that forgets it fail here rather than quietly picking a different
// hart count than the one on the configure command line.
//
// The single hart bound in the kernel. One number sizes platform::Info's FDT
// hart list (platform/platform.h), the per-hart `g_cpus[]` array in hart.cc and
// the boot stack pool (boot/stack.h), so none of those bounds can drift apart:
// every dense cpuid, and g_cpus[] is indexed by exactly that cpuid. start.cc
// static_asserts the pool equality. Kernel stacks live in the shared root-508
// arena (one slot per thread, see mm/vm.h), so MAX_HARTS does not bound a
// stack window.
#ifndef SEELE_MAX_HARTS
#error "SEELE_MAX_HARTS is not defined: build the kernel through CMake (see CMakeLists.txt)"
#endif

constexpr std::size_t MAX_HARTS = SEELE_MAX_HARTS;

struct cpu {
    uint64_t hartid;

    // Interrupt-disable bookkeeping for the current hart.
    //
    // `interrupt_depth` counts how many locks the current hart holds, and
    // `interrupts_were_enabled` remembers sstatus.SIE from before the outermost
    // lock(), so that only the outermost unlock() has to restore it.
    uint32_t interrupt_depth = 0;
    bool interrupts_were_enabled = false;

    // Fallback thread when this hart has no ready kernel thread. Belongs to
    // `proc::kernel_proc()` and has its own context and stack slot; never
    // placed on the ready or sleeping list. Null until boot creates it.
    proc::thread* idle = nullptr;

    // This hart's currently running thread. kmain() points it at the idle
    // thread just before switch_thread() enters it, so the hart always has a
    // current thread once it leaves the boot context; the scheduler updates it
    // on every switch.
    proc::thread* current = nullptr;

    // Per-hart scratch area loaded into sscratch while executing in U-mode.
    // `kernel_tp` is initialized once per hart in kmain(); `kernel_sp` is
    // updated by tvecret before each return to U-mode.
    trap::trap_scratch trap_scratch{};

    Spinlock lock{};
    IntrusiveList<proc::thread> ready{};
    IntrusiveList<proc::thread> sleeping{};
};

// The current hart's dense index, 0..MAX_HARTS-1.
//
// _entry (start.S) assigns it before any C++ code runs: hart 0 is the boot hart
// and keeps 0, every other hart takes the next value from an atomic
// arrival-order counter. The hardware hartid is not this index -- firmware is
// allowed to hand out sparse or large hartids -- so a hart that needs the
// hardware id keeps the value the firmware passed to it (kmain receives it in
// its first argument). cpuid() is what indexes g_cpus[] and what spinlocks store
// as their owner.
uint64_t cpuid() noexcept;

cpu* mycpu() noexcept;
