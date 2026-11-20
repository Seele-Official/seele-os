#include "proc/process.h"

#include <atomic>
#include <cstdint>
#include <new>

#include "console.h"
#include "hart.h"
#include "panic.h"
#include "riscv.h"
#include "mm/heap.h"
#include "mm/vm.h"

// The process is one heap object, so its size has to stay inside the heap's
// largest class. The bound makes a layout change fail the build instead of
// silently invalidating that sentence (and the class the struct is served from).
static_assert(sizeof(proc::process) <= heap::MAX_SIZE,
              "the process must fit in one heap size class");

namespace proc {

process* make_proc(const Paddr pagetable) noexcept {
    // The active page table must expose the shared heap before allocating the
    // process object. Ordinary new panics on exhaustion.
    process* created = new process;

    // Keep the ready-to-load CSR value (mode bits included) next to the raw
    // root: shed/switch.S writes `satp` straight to the CSR when a thread of
    // this process is scheduled, while the page-table walk keeps using
    // `pagetable`.
    created->satp = riscv::csr::satp::MODE_SV39 | (to_uint64(pagetable) >> riscv::PGSHIFT);
    created->pagetable = pagetable;
    created->thread_count = 0;

    LOG_INFO("proc: create process at {:#018x}, pagetable {:#018x}, kstack slots {}",
             to_uint64(vaddr(created)),
             to_uint64(pagetable),
             vm::k::KSTACK_MAX_THREADS);

    return created;
}

// The thread is one heap object, so its size has to stay inside the heap's
// largest class; a layout change fails the build instead of silently changing
// which size class serves it.
static_assert(sizeof(proc::thread) <= heap::MAX_SIZE, "the thread must fit in one heap size class");

// Thread construction uses the shared heap and the owner's private stack
// subtree. It does not publish the thread to a run queue.
thread* make_thread(process* owner, void (*entry)()) noexcept {
    // The per-process counter is the slot index of this thread's kernel stack.
    // Every thread gets its own slot, so a suspended thread's frames stay on
    // its own stack while another thread of the process runs.
    const uint64_t slot = owner->thread_count.fetch_add(1, std::memory_order_relaxed);
    SEELE_ASSERT(slot < vm::k::KSTACK_MAX_THREADS,
                 "proc: no kernel stack slot left ({} of {} used)",
                 slot,
                 vm::k::KSTACK_MAX_THREADS);

    // Allocate and map the slot before the thread can ever run. The mapping
    // targets the process's private stack subtree; the guard page below the
    // slot stays unmapped, so an overflow faults instead of reaching the
    // previous thread's stack.
    vm::k::map_thread_stack(owner->pagetable, slot);

    thread* created = new proc::thread;

    created->proc = owner;
    created->context = kernel_context{};
    created->context.ra = reinterpret_cast<uintptr_t>(entry);
    created->context.sp = to_uint64(vm::k::kstack_slot_top(slot));
    created->kstack.base = vm::k::kstack_slot_base(slot);
    created->kstack.size = vm::k::KSTACK_SIZE;

    LOG_DEBUG("proc: thread {:#018x} slot {}, kstack [{:#018x}, {:#018x})",
              to_uint64(vaddr(created)),
              slot,
              to_uint64(created->kstack.base),
              to_uint64(created->kstack.base) + created->kstack.size);

    return created;
}

}  // namespace proc
