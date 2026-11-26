#pragma once

// Processes own the address space and exit status; threads are lightweight
// scheduling units with no per-thread exit state or exit code.
//
// All kernel threads across every hart belong to the single kernel-lifetime
// system process (`kernel_proc()`), whose root page table is `vm::k::pagetable()`.
// User processes get their own Sv39 page-table root (copying frozen kernel root
// entries from `vm::k::pagetable()`) and are lifetime-managed by atomic
// reference counting: external callers hold `ProcessHandle`s, and each live
// thread in the process holds one internal reference.
//
// When a thread exits, `sched::exit_current()` parks it on the hart's
// `dying_thread` slot and `proc::reap_dying()` immediately reclaims it once the
// hart has switched to another thread's stack and address space. When the last
// live thread of a user process is reaped, the process immediately frees its
// user mappings and root page table, publishes `ProcessState::Exited`, wakes
// all `wait()` callers, and releases the thread's reference (destroying the
// `process` struct immediately if no `ProcessHandle` remains).

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "concurrency/latch.h"
#include "concurrency/spinlock.h"
#include "mm/addr.h"

struct cpu;

namespace proc {

struct thread;

// Lifecycle state of a process. `kernel_proc()` remains `Alive` forever.
enum class ProcessState : uint32_t {
    Alive,    // running (or newly created)
    Exiting,  // termination reason recorded; waiting for its thread(s) to exit
    Exited,   // all threads reaped, user address space freed, exit status final
};

// Final termination status of a user process.
struct exit_status {
    int64_t code = 0;          // status passed to EXIT_GROUP, or -1 on unhandled user fault
    uint64_t fault_cause = 0;  // 0 on normal exit; scause on unhandled user fault
};

// One process: address space, reference-counted lifetime, live thread count,
// synchronized exit state, and one-shot exit completion latch.
//
// `satp` is the full CSR value to load -- MODE_SV39 | (pagetable >> PGSHIFT) --
// kept at offset 0 so `sched/switch.S` can load it with a single `ld` before
// writing `satp`. `pagetable` is the physical address of the process's Sv39
// root page table, or `null_paddr()` once reclaimed on exit.
struct process {
    uint64_t satp;
    Paddr pagetable;
    std::atomic<uint64_t> ref_count;
    std::atomic<uint64_t> thread_count;
    Spinlock state_lock;
    Latch exited;
    ProcessState state;
    int64_t exit_code;
    uint64_t fault_cause;
};

static_assert(offsetof(process, satp) == 0, "switch.S reads satp at the start of process");
static_assert(std::atomic<uint64_t>::is_always_lock_free,
              "process ref/thread counts must not require libatomic helpers");

// Reference-counted RAII handle to a user `proc::process`.
//
// Copying increments `ref_count`; moving transfers ownership without touching
// `ref_count`; destroying or calling `reset()` decrements `ref_count` and
// destroys the `process` object when the last reference drops. Never wraps
// `kernel_proc()`.
class ProcessHandle {
public:
    constexpr ProcessHandle() noexcept = default;
    ~ProcessHandle() noexcept;

    ProcessHandle(const ProcessHandle& other) noexcept;
    ProcessHandle& operator= (const ProcessHandle& other) noexcept;
    ProcessHandle(ProcessHandle&& other) noexcept;
    ProcessHandle& operator= (ProcessHandle&& other) noexcept;

    [[nodiscard]] explicit operator bool() const noexcept {
        return m_proc != nullptr;
    }

    [[nodiscard]] process* get() const noexcept {
        return m_proc;
    }

    [[nodiscard]] process* operator->() const noexcept {
        return m_proc;
    }

    // Release this handle's reference and reset to empty.
    void reset() noexcept;

    // Block the calling non-idle thread until the referenced process reaches
    // `ProcessState::Exited` and return its final `exit_status`. Requires local
    // interrupts enabled, no spinlock held, a non-empty handle, and a caller
    // outside the target process.
    [[nodiscard]] exit_status wait() const noexcept;

    // Return the final `exit_status` if the referenced process has reached
    // `ProcessState::Exited`, or `std::nullopt` while still `Alive`/`Exiting`.
    [[nodiscard]] std::optional<exit_status> try_status() const noexcept;

private:
    friend ProcessHandle make_proc(Paddr pagetable) noexcept;

    explicit ProcessHandle(process* proc) noexcept : m_proc(proc) {}

    process* m_proc = nullptr;
};

// Initialize the single kernel-lifetime system process around `kernel_pagetable`
// (`vm::k::pagetable()`). Called once on the boot hart before creating threads.
void init(Paddr kernel_pagetable) noexcept;

// The single system process shared by all kernel threads across every hart.
[[nodiscard]] process* kernel_proc() noexcept;

// Allocate a new user process object on the kernel heap around an initialized
// private root `pagetable` (from `vm::k::create_process_pagetable()`). Returns
// a `ProcessHandle` owning the initial reference (`ref_count == 1`). If all
// references are released before any thread runs, the destructor also reclaims
// `pagetable`. Panics on OOM.
[[nodiscard]] ProcessHandle make_proc(Paddr pagetable) noexcept;

// Create a kernel thread in `owner` (must be `kernel_proc()`) with a globally
// unique guarded kernel stack slot and an initial context entering `entry` via
// the internal trampoline. If `entry` returns, the thread exits and is reaped
// automatically (`idle` must not return). Does not enqueue the thread.
//
// Requires `vm::k::enable()` on the calling hart. Panics on stack-slot or heap
// exhaustion.
[[nodiscard]] thread* make_thread(process* owner, void (*entry)()) noexcept;

// Create a user-mode thread in `owner` (must be a live single-threaded user
// process with `thread_count == 0`). Retains an internal process reference for
// the thread, increments `owner->thread_count`, and prepares an initial
// `trap::trap_frame` at the top of its kernel stack (`sepc = entry_pc`,
// `sp = user_sp` 16-byte aligned, `SPP = 0`, `SIE = 0`, `SPIE = 1`). Does not
// enqueue the thread; panics on stack-slot or heap exhaustion.
[[nodiscard]] thread* make_user_thread(process* owner, Vaddr entry_pc, Vaddr user_sp) noexcept;

// Terminate the current user process with `(code, fault_cause)` and retire the
// current thread. First termination reason wins (`Alive -> Exiting`). Never
// returns; must not be called from `kernel_proc()`.
[[noreturn]] void exit_current_process(int64_t code, uint64_t fault_cause = 0) noexcept;

// Reclaim any exited thread parked in `mycpu()->dying_thread` after switching
// off its kernel stack and address space. No-op when `dying_thread == nullptr`.
// Must be called with local interrupts disabled and no spinlock held.
void reap_dying() noexcept;

// Current number of allocated user `process` objects not yet destroyed.
[[nodiscard]] uint64_t user_process_count() noexcept;

// Switch context: return address, stack pointer and callee-saved integer
// registers. The return address lets a resumed thread continue after its call
// to switch_thread(); a new thread instead enters its trampoline. Trap entry
// separately preserves the interrupted register state on the kernel stack.
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

// Explicit scheduler lifecycle of a thread.
//
// `Blocking` is the two-phase sleep intermediate state set by
// sched::prepare_sleep() while local interrupts are disabled, before the
// thread exposes itself to a waker. A concurrent sched::wake() that arrives
// before sched::sleep() acquires the target hart's lock flips `Blocking` back
// to `Running`, so sched::sleep() returns immediately without losing the wake.
// Exiting threads do not transition to a parked state: `sched::exit_current()`
// hands them to `cpu::dying_thread` for immediate reclamation on the next
// thread's stack. Stored as uint32_t so RV64A word atomics operate natively
// without sub-word masking loops.
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
// a process. Kernel threads point at `kernel_proc()` and store their entry
// function in `entry`; user threads leave `entry == nullptr` and enter U-mode
// via the initial `trap_frame` at the top of `kstack`. `hart` points at the
// per-hart `cpu` that owns this thread's run queue, or nullptr while `Created`.
struct thread {
    process* proc;
    kernel_context context;
    kernel_stack kstack;
    void (*entry)() = nullptr;
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
static_assert(offsetof(thread, proc) == 0, "switch.S reads process pointer at offset 0");
static_assert(offsetof(thread, context) == sizeof(process*),
              "switch.S reads the context after process pointer");

}  // namespace proc
