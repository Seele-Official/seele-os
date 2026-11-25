#pragma once

// Processes provide the address space; threads are the scheduling units.
// All kernel threads across every hart belong to the single kernel-lifetime
// system process (`kernel_proc()`), whose root page table is `vm::k::pagetable()`.
//
// Additional processes get their own page-table root (copying frozen kernel
// root entries from `vm::k::pagetable()`), while thread kernel stacks are
// allocated globally from the shared `vm::k::KSTACK_WINDOW_BASE` arena so every
// thread's stack has a unique virtual address across all harts and processes.
// `switch_thread()` reloads `satp` only when switching across distinct processes.

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "mm/addr.h"

struct cpu;

namespace proc {

struct thread;

// One process: an address space and its thread count.
//
// `satp` is the full CSR value to load -- MODE_SV39 | (pagetable >> PGSHIFT) --
// kept at offset 0 so `sched/switch.S` can load it with a single `ld` before
// writing `satp`. `pagetable` is the physical address of the process's Sv39
// root page table. `thread_count` tracks threads created in this process.
struct process {
    uint64_t satp;
    Paddr pagetable;
    std::atomic<uint64_t> thread_count;
};

static_assert(offsetof(process, satp) == 0, "switch.S reads satp at the start of process");

// Initialize the single kernel-lifetime system process around `kernel_pagetable`
// (`vm::k::pagetable()`). Called once on the boot hart before creating threads.
void init(Paddr kernel_pagetable) noexcept;

// The single system process shared by all kernel threads across every hart.
[[nodiscard]] process* kernel_proc() noexcept;

// Create a thread in `owner` with a globally unique guarded kernel stack slot
// and an initial context that enters `entry`. Does not enqueue or run the
// thread; its entry must not return. Kernel thread callers pass `kernel_proc()`.
//
// Requires `vm::k::enable()` on the calling hart (heap and shared stack window
// accessible). Panics on stack-slot or heap exhaustion.
[[nodiscard]] thread* make_thread(process* owner, void (*entry)()) noexcept;

// Allocate a new process object on the kernel heap around an initialized root
// `pagetable` (e.g. from `vm::k::create_process_pagetable()`). Does not activate
// the page table or create threads; panics on OOM.
[[nodiscard]] process* make_proc(Paddr pagetable) noexcept;

// Switch context: return address, stack pointer and callee-saved integer
// registers. The return address lets a resumed thread continue after its call
// to switch_thread(); a new thread instead enters its configured entry point.
// Trap entry separately preserves the interrupted register state on the kernel
// stack.
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

// Explicit scheduler lifecycle of a kernel thread.
//
// `Blocking` is the two-phase sleep intermediate state set by
// sched::prepare_sleep() while local interrupts are disabled, before the
// thread exposes itself to a waker. A concurrent sched::wake() that arrives
// before sched::sleep() acquires the target hart's lock flips `Blocking` back
// to `Running`, so sched::sleep() returns immediately without losing the wake.
// Stored as uint32_t so RV64A word atomics operate natively without sub-word
// masking loops.
enum class ThreadState : uint32_t {
    Created,   // newly allocated; not yet enqueued or entered
    Ready,     // runnable on its bound hart (in `ready`, or parked idle)
    Running,   // executing on its bound hart (`cpu->current == this`)
    Blocking,  // preparing to sleep with local interrupts disabled
    Blocked,   // parked in its bound hart's `sleeping` queue until woken
};

[[nodiscard]] constexpr const char* thread_state_name(ThreadState state) noexcept {
    switch(state) {
        case ThreadState::Created: return "created";
        case ThreadState::Ready: return "ready";
        case ThreadState::Running: return "running";
        case ThreadState::Blocking: return "blocking";
        case ThreadState::Blocked: return "blocked";
    }
    return "unknown";
}

// Scheduling unit: a resumable context and globally unique kernel stack within
// a process. Kernel threads point at `kernel_proc()`. Interrupt preemption and
// voluntary sleep/yield preserve each thread's frames on its own stack. `hart`
// points at the per-hart `cpu` that owns this thread's run queue, or nullptr
// while the thread is still in `Created`.
struct thread {
    process* proc;
    kernel_context context;
    kernel_stack kstack;
    std::atomic<ThreadState> state{ThreadState::Created};
    std::atomic<::cpu*> hart{nullptr};
    thread* prev = nullptr;
    thread* next = nullptr;
};

static_assert(std::atomic<ThreadState>::is_always_lock_free,
              "ThreadState atomics must not require libatomic helpers");
static_assert(std::atomic<::cpu*>::is_always_lock_free,
              "hart pointer atomics must not require libatomic helpers");

static_assert(offsetof(kernel_context, ra) == 0);
static_assert(offsetof(kernel_context, sp) == 8);
static_assert(offsetof(kernel_context, s) == 16);
static_assert(sizeof(kernel_context) == 8 * 14);
static_assert(offsetof(thread, context) == sizeof(process*),
              "switch.S reads the context after process pointer ");

}  // namespace proc
