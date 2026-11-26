#include "proc/process.h"

#include <atomic>
#include <cstdint>
#include <new>
#include <optional>
#include <type_traits>

#include "console.h"
#include "hart.h"
#include "panic.h"
#include "riscv.h"
#include "trap.h"
#include "concurrency/lock_guard.h"
#include "mm/heap.h"
#include "mm/kalloc.h"
#include "mm/vm.h"
#include "sched/schedule.h"

static_assert(sizeof(proc::process) <= heap::MAX_SIZE,
              "the process must fit in one heap size class");
static_assert(std::is_trivially_destructible_v<proc::process>,
              "proc::process must be trivially destructible for static storage");
static_assert(sizeof(proc::thread) <= heap::MAX_SIZE, "the thread must fit in one heap size class");

namespace proc {
namespace {

// Single kernel-lifetime system process in static storage (avoids spending a
// heap slab page on a permanent singleton).
process g_kernel_process{};
bool g_kernel_process_ready = false;

// Live heap-allocated user processes (incremented in make_proc, decremented on
// final destruction when ref_count reaches 0).
std::atomic<uint64_t> g_user_process_count{0};

void retain_ref(process* target) noexcept {
    SEELE_ASSERT(target != nullptr && target != kernel_proc(),
                 "proc: retain_ref() requires a user process");
    const uint64_t prev = target->ref_count.fetch_add(1, std::memory_order_relaxed);
    SEELE_ASSERT(prev > 0, "proc: retaining user process with zero ref_count");
}

void destroy_process(process* target) noexcept {
    SEELE_ASSERT(target != nullptr && target != kernel_proc(),
                 "proc: destroy_process() requires a user process");
    SEELE_ASSERT(target->thread_count.load(std::memory_order_acquire) == 0,
                 "proc: destroying user process with live threads");

    // Reclaim the address space if this process was dropped before any thread
    // ever exited through reap_thread().
    if(!is_null(target->pagetable)) {
        const Paddr pt = target->pagetable;
        target->pagetable = null_paddr();
        target->satp = 0;
        vm::u::destroy_user_mappings(pt);
        phymem::free(pt);
    }

    const uint64_t prev = g_user_process_count.fetch_sub(1, std::memory_order_acq_rel);
    SEELE_ASSERT(prev > 0, "proc: user_process_count underflow");

    LOG_DEBUG("proc: destroy process {:#018x}", to_uint64(vaddr(target)));
    delete target;
}

void release_ref(process* target) noexcept {
    SEELE_ASSERT(target != nullptr && target != kernel_proc(),
                 "proc: release_ref() requires a user process");
    const uint64_t prev = target->ref_count.fetch_sub(1, std::memory_order_acq_rel);
    SEELE_ASSERT(prev > 0, "proc: ref_count underflow");
    if(prev == 1) {
        destroy_process(target);
    }
}

// First-time entry for a kernel thread. Reached via switch_thread() with local
// interrupts disabled (`SIE == 0`). Reaps any thread that just exited on this
// hart before entering `entry`, and retires the thread automatically if `entry`
// returns.
[[noreturn]] void kernel_thread_trampoline() noexcept {
    SEELE_ASSERT(!riscv::is_intr_on(), "proc: kernel trampoline entered with interrupts enabled");
    reap_dying();

    cpu* const hart = mycpu();
    SEELE_ASSERT(hart->current != nullptr && hart->current->entry != nullptr,
                 "proc: kernel trampoline without entry");
    void (*const entry)() = hart->current->entry;
    entry();

    riscv::intr_off();
    sched::exit_current();
}

// First-time kernel entry for a user-mode thread. Reached via switch_thread()
// with local interrupts disabled (`SIE == 0`) and `satp` already switched to
// the user process page table. Reaps any thread that just exited on this hart,
// then hands the pre-populated trap_frame at the top of this thread's kernel
// stack directly to `trap::enter_user()`.
[[noreturn]] void user_thread_trampoline() noexcept {
    SEELE_ASSERT(!riscv::is_intr_on(), "proc: user trampoline entered with interrupts enabled");
    reap_dying();

    cpu* const hart = mycpu();
    SEELE_ASSERT(hart->current != nullptr, "proc: user trampoline without current thread");

    const Vaddr kstack_top = hart->current->kstack.base + hart->current->kstack.size;
    auto* const frame = to_pointer<trap::trap_frame>(kstack_top - sizeof(trap::trap_frame));
    trap::enter_user(frame, &hart->trap_scratch);
}

}  // namespace

ProcessHandle::~ProcessHandle() noexcept {
    reset();
}

ProcessHandle::ProcessHandle(const ProcessHandle& other) noexcept : m_proc(other.m_proc) {
    if(m_proc != nullptr) {
        retain_ref(m_proc);
    }
}

ProcessHandle& ProcessHandle::operator= (const ProcessHandle& other) noexcept {
    if(this == &other) {
        return *this;
    }
    if(other.m_proc != nullptr) {
        retain_ref(other.m_proc);
    }
    reset();
    m_proc = other.m_proc;
    return *this;
}

ProcessHandle::ProcessHandle(ProcessHandle&& other) noexcept : m_proc(other.m_proc) {
    other.m_proc = nullptr;
}

ProcessHandle& ProcessHandle::operator= (ProcessHandle&& other) noexcept {
    if(this == &other) {
        return *this;
    }
    reset();
    m_proc = other.m_proc;
    other.m_proc = nullptr;
    return *this;
}

void ProcessHandle::reset() noexcept {
    if(m_proc != nullptr) {
        process* const target = m_proc;
        m_proc = nullptr;
        release_ref(target);
    }
}

exit_status ProcessHandle::wait() const noexcept {
    SEELE_ASSERT(m_proc != nullptr, "proc: ProcessHandle::wait() on empty handle");
    const cpu* hart = mycpu();
    SEELE_ASSERT(hart->current != nullptr && hart->current->proc != m_proc,
                 "proc: process cannot wait on itself");

    // Latch::wait() synchronizes with the count_down() performed after final
    // exit_code / fault_cause publication in reap_dying().
    m_proc->exited.wait();
    return exit_status{m_proc->exit_code, m_proc->fault_cause};
}

std::optional<exit_status> ProcessHandle::try_status() const noexcept {
    SEELE_ASSERT(m_proc != nullptr, "proc: ProcessHandle::try_status() on empty handle");

    if(!m_proc->exited.try_wait()) {
        return std::nullopt;
    }
    return exit_status{m_proc->exit_code, m_proc->fault_cause};
}

void init(Paddr kernel_pagetable) noexcept {
    SEELE_ASSERT(!g_kernel_process_ready, "proc: init() called more than once");
    SEELE_ASSERT(!is_null(kernel_pagetable), "proc: null kernel pagetable");

    g_kernel_process.satp = vm::k::make_satp(kernel_pagetable);
    g_kernel_process.pagetable = kernel_pagetable;
    g_kernel_process.ref_count.store(1, std::memory_order_relaxed);
    g_kernel_process.thread_count.store(0, std::memory_order_relaxed);
    g_kernel_process.state = ProcessState::Alive;
    g_kernel_process.exit_code = 0;
    g_kernel_process.fault_cause = 0;
    g_kernel_process_ready = true;

    LOG_INFO("proc: system process at {:#018x}, pagetable {:#018x}",
             to_uint64(vaddr(&g_kernel_process)),
             to_uint64(kernel_pagetable));
}

process* kernel_proc() noexcept {
    SEELE_ASSERT(g_kernel_process_ready, "proc: kernel_proc() before init()");
    return &g_kernel_process;
}

ProcessHandle make_proc(Paddr pagetable) noexcept {
    SEELE_ASSERT(!is_null(pagetable) && pagetable != vm::k::pagetable(),
                 "proc: make_proc() requires a private user pagetable");

    process* created = new process{};
    created->satp = vm::k::make_satp(pagetable);
    created->pagetable = pagetable;
    created->ref_count.store(1, std::memory_order_relaxed);
    created->thread_count.store(0, std::memory_order_relaxed);
    created->state = ProcessState::Alive;
    created->exit_code = 0;
    created->fault_cause = 0;
    g_user_process_count.fetch_add(1, std::memory_order_relaxed);

    LOG_INFO("proc: create process at {:#018x}, pagetable {:#018x}",
             to_uint64(vaddr(created)),
             to_uint64(pagetable));

    return ProcessHandle{created};
}

thread* make_thread(process* owner, void (*entry)()) noexcept {
    SEELE_ASSERT(owner == kernel_proc(), "proc: make_thread() requires kernel_proc()");
    SEELE_ASSERT(entry != nullptr, "proc: make_thread() with null entry");

    const std::optional<Vaddr> stack_base = vm::k::alloc_kernel_stack();
    if(!stack_base.has_value()) {
        SEELE_PANIC("proc: out of kernel stack slots");
    }

    thread* created = new proc::thread;
    const uint64_t live_threads = owner->thread_count.fetch_add(1, std::memory_order_relaxed) + 1;

    created->proc = owner;
    created->context = kernel_context{};
    created->context.ra = reinterpret_cast<uintptr_t>(&kernel_thread_trampoline);
    created->context.sp = to_uint64(*stack_base + vm::k::KSTACK_SIZE);
    created->kstack.base = *stack_base;
    created->kstack.size = vm::k::KSTACK_SIZE;
    created->entry = entry;

    LOG_DEBUG(
        "proc: kernel thread {:#018x} (live={} in proc {:#018x}), kstack [{:#018x}, {:#018x})",
        to_uint64(vaddr(created)),
        live_threads,
        to_uint64(vaddr(owner)),
        to_uint64(created->kstack.base),
        to_uint64(created->kstack.base) + created->kstack.size);

    return created;
}

thread* make_user_thread(process* owner, Vaddr entry_pc, Vaddr user_sp) noexcept {
    SEELE_ASSERT(owner != nullptr && owner != kernel_proc(),
                 "proc: make_user_thread() requires a user process");
    SEELE_ASSERT(to_uint64(user_sp) % 16 == 0,
                 "proc: user stack pointer {:#018x} is not 16-byte aligned",
                 to_uint64(user_sp));

    const std::optional<Vaddr> stack_base = vm::k::alloc_kernel_stack();
    if(!stack_base.has_value()) {
        SEELE_PANIC("proc: out of kernel stack slots");
    }

    thread* created = new proc::thread;
    uint64_t live_threads = 0;
    {
        LockGuard guard{owner->state_lock};
        SEELE_ASSERT(owner->state == ProcessState::Alive,
                     "proc: cannot add thread to an exiting or exited process");
        SEELE_ASSERT(owner->thread_count.load(std::memory_order_relaxed) == 0,
                     "proc: multi-threaded user processes are not supported yet");
        retain_ref(owner);
        live_threads = owner->thread_count.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    const Vaddr kstack_top = *stack_base + vm::k::KSTACK_SIZE;
    const Vaddr frame_va = kstack_top - sizeof(trap::trap_frame);
    auto* const frame = to_pointer<trap::trap_frame>(frame_va);
    *frame = trap::trap_frame{};
    frame->sepc = to_uint64(entry_pc);
    frame->sp = to_uint64(user_sp);
    const uint64_t status = riscv::csrr(riscv::registers::sstatus);
    frame->sstatus = (status & ~(riscv::csr::sstatus::SPP | riscv::csr::sstatus::SIE)) |
                     riscv::csr::sstatus::SPIE;

    created->proc = owner;
    created->context = kernel_context{};
    created->context.ra = reinterpret_cast<uintptr_t>(&user_thread_trampoline);
    created->context.sp = to_uint64(frame_va);
    created->kstack.base = *stack_base;
    created->kstack.size = vm::k::KSTACK_SIZE;
    created->entry = nullptr;

    LOG_DEBUG("proc: user thread {:#018x} (live={} in proc {:#018x}), entry {:#018x}, usp {:#018x}",
              to_uint64(vaddr(created)),
              live_threads,
              to_uint64(vaddr(owner)),
              to_uint64(entry_pc),
              to_uint64(user_sp));

    return created;
}

[[noreturn]] void exit_current_process(int64_t code, uint64_t fault_cause) noexcept {
    proc::thread* const current = mycpu()->current;
    SEELE_ASSERT(current != nullptr && current->proc != nullptr,
                 "proc: exit_current_process() without current thread or process");
    process* const owner = current->proc;
    SEELE_ASSERT(owner != kernel_proc(), "proc: cannot exit kernel_proc()");

    {
        LockGuard guard{owner->state_lock};
        if(owner->state == ProcessState::Alive) {
            owner->state = ProcessState::Exiting;
            owner->exit_code = code;
            owner->fault_cause = fault_cause;
        }
    }

    riscv::intr_off();
    sched::exit_current();
}

void reap_dying() noexcept {
    cpu* const hart = mycpu();
    SEELE_ASSERT(!riscv::is_intr_on(), "proc::reap_dying() with interrupts enabled");
    SEELE_ASSERT(hart->interrupt_depth == 0, "proc::reap_dying() while holding a spinlock");

    thread* const dead = hart->dying_thread;
    if(dead == nullptr) {
        return;
    }
    hart->dying_thread = nullptr;
    SEELE_ASSERT(dead != hart->current, "proc::reap_dying(): cannot reap current thread");

    process* const owner = dead->proc;
    SEELE_ASSERT(owner != nullptr, "proc::reap_dying() without owner process");

    vm::k::free_kernel_stack(dead->kstack.base);
    delete dead;

    if(owner == kernel_proc()) {
        const uint64_t prev = owner->thread_count.fetch_sub(1, std::memory_order_relaxed);
        SEELE_ASSERT(prev > 0, "proc: kernel_proc thread_count underflow");
        return;
    }

    const uint64_t prev_threads = owner->thread_count.fetch_sub(1, std::memory_order_acq_rel);
    SEELE_ASSERT(prev_threads > 0, "proc: user process thread_count underflow");

    if(prev_threads == 1) {
        // No thread remains in this process and switch_thread() has already
        // switched satp away with sfence.vma, so its user address space can be
        // reclaimed immediately.
        const Paddr pt = owner->pagetable;
        owner->pagetable = null_paddr();
        owner->satp = 0;
        if(!is_null(pt)) {
            vm::u::destroy_user_mappings(pt);
            phymem::free(pt);
        }

        {
            LockGuard guard{owner->state_lock};
            if(owner->state == ProcessState::Alive) {
                owner->exit_code = 0;
                owner->fault_cause = 0;
            }
            owner->state = ProcessState::Exited;
        }

        // Open the exit latch outside state_lock so state_lock never nests with
        // the latch or scheduler locks.
        owner->exited.count_down();
    }

    release_ref(owner);
}

uint64_t user_process_count() noexcept {
    return g_user_process_count.load(std::memory_order_acquire);
}

}  // namespace proc
