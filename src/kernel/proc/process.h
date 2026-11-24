#pragma once

// Processes provide the address space; threads are the scheduling units.
// Kernel threads are created in a system process. Until cross-hart scheduling
// is implemented, each hart has its own system process, shared by all of its
// kernel threads rather than reserved for idle.
//
// Each process has a page-table root and a private kernel stack window. Threads
// share the address space but use distinct stack slots, so a suspended thread's
// frames remain intact. switch_thread() changes satp only across processes.

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "trap.h"
#include "mm/addr.h"

namespace proc {

struct thread;

// One kernel process: an address space and the stack accounting of its threads.
//
// `satp` is the full CSR value to load -- MODE_SV39 | (pagetable >> PGSHIFT),
// built by make_proc() -- kept ready so sched/switch.S can write it with a
// single csrw when a thread of this process is scheduled. It must sit at
// offset 0: the assembly reads it with `ld t0, (t1)` before the satp write,
// and the static_assert below pins that. `pagetable` is the physical address
// of the process's Sv39 root page table; it stays a Paddr because the page is
// named physically in the page-table walk and in satp, while the struct itself
// lives at a heap virtual address.
//
// `thread_count` reserves successive stack slots; slots are not reclaimed.
// make_thread() rejects a slot at or beyond vm::k::KSTACK_MAX_THREADS.
struct process {
    uint64_t satp;
    Paddr pagetable;
    std::atomic<uint64_t> thread_count;
};

static_assert(offsetof(process, satp) == 0, "switch.S reads satp at the start of process");

// Create a thread in `owner` with its own mapped kernel stack and an initial
// context that enters `entry`. Does not enqueue or run the thread; its entry
// must not return. Kernel thread callers use their hart's system process.
//
// Requires an initialized owner page table and access to the kernel heap.
// Reserves the next stack slot permanently; slot exhaustion or allocation
// failure panics. Current callers create threads only during boot.
[[nodiscard]] thread* make_thread(process* owner, void (*entry)()) noexcept;

// Create a process around an initialized page-table root; does not build or
// activate the page table, create threads, or schedule anything. The caller
// must already have access to the kernel heap. Allocation failure panics.
//
// Boot uses this to create each hart's system process, the address space for
// all kernel threads created on that hart. Idle is one such thread, not a
// separate kind of process.
[[nodiscard]] process* make_proc(const Paddr pagetable) noexcept;

// Switch context: return address, stack pointer and callee-saved integer
// registers. The return address lets a resumed thread continue after its call
// to switch_thread(); a new thread instead enters its configured entry point.
// Trap entry separately preserves the interrupted caller-saved registers.
//
// The layout is part of the contract with sched/switch.S: the assembly reads
// and writes these fields at fixed offsets from a thread pointer, and the
// static_asserts below pin them. Do not reorder the members.
struct kernel_context {
    uint64_t ra;     // offset 0:  the pc the context returns to
    uint64_t sp;     // offset 8:  the thread's kernel stack
    uint64_t s[12];  // offset 16: s0..s11, i.e. s[i] is at 16 + i * 8
};

// The mapped portion of one thread's stack slot; the guard page is excluded.
struct kernel_stack {
    Vaddr base;     // lowest mapped byte of the thread's stack slot
    uint64_t size;  // mapped bytes; the stack grows down from base + size
};

// Scheduling unit: a resumable context and private stack within a process.
// Kernel threads point at their hart's system process. Interrupt preemption
// and voluntary sleep/yield preserve each thread's frames on its own stack.
struct thread {
    process* proc;
    kernel_context context;
    kernel_stack kstack;
    trap::trap_frame frame;
    thread* prev = nullptr;
    thread* next = nullptr;
};

static_assert(offsetof(kernel_context, ra) == 0);
static_assert(offsetof(kernel_context, sp) == 8);
static_assert(offsetof(kernel_context, s) == 16);
static_assert(sizeof(kernel_context) == 8 * 14);
static_assert(offsetof(thread, context) == sizeof(process*),
              "switch.S reads the context after process pointer ");

}  // namespace proc
