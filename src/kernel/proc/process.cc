#include "proc/process.h"

#include <atomic>
#include <cstdint>
#include <new>
#include <optional>
#include <type_traits>

#include "console.h"
#include "panic.h"
#include "mm/heap.h"
#include "mm/vm.h"

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

}  // namespace

void init(Paddr kernel_pagetable) noexcept {
    SEELE_ASSERT(!g_kernel_process_ready, "proc: init() called more than once");
    SEELE_ASSERT(!is_null(kernel_pagetable), "proc: null kernel pagetable");

    g_kernel_process.satp = vm::k::make_satp(kernel_pagetable);
    g_kernel_process.pagetable = kernel_pagetable;
    g_kernel_process.thread_count.store(0, std::memory_order_relaxed);
    g_kernel_process_ready = true;

    LOG_INFO("proc: system process at {:#018x}, pagetable {:#018x}",
             to_uint64(vaddr(&g_kernel_process)),
             to_uint64(kernel_pagetable));
}

process* kernel_proc() noexcept {
    SEELE_ASSERT(g_kernel_process_ready, "proc: kernel_proc() before init()");
    return &g_kernel_process;
}

process* make_proc(Paddr pagetable) noexcept {
    SEELE_ASSERT(!is_null(pagetable), "proc: make_proc() with null pagetable");

    process* created = new process;
    created->satp = vm::k::make_satp(pagetable);
    created->pagetable = pagetable;
    created->thread_count.store(0, std::memory_order_relaxed);

    LOG_INFO("proc: create process at {:#018x}, pagetable {:#018x}",
             to_uint64(vaddr(created)),
             to_uint64(pagetable));

    return created;
}

thread* make_thread(process* owner, void (*entry)()) noexcept {
    SEELE_ASSERT(owner != nullptr, "proc: make_thread() without an owner process");

    const std::optional<Vaddr> stack_base = vm::k::alloc_kernel_stack();
    if(!stack_base.has_value()) {
        SEELE_PANIC("proc: out of kernel stack slots");
    }

    thread* created = new proc::thread;
    const uint64_t thread_num = owner->thread_count.fetch_add(1, std::memory_order_relaxed) + 1;

    created->proc = owner;
    created->context = kernel_context{};
    created->context.ra = reinterpret_cast<uintptr_t>(entry);
    created->context.sp = to_uint64(*stack_base + vm::k::KSTACK_SIZE);
    created->kstack.base = *stack_base;
    created->kstack.size = vm::k::KSTACK_SIZE;

    LOG_DEBUG("proc: thread {:#018x} (#{} in proc {:#018x}), kstack [{:#018x}, {:#018x})",
              to_uint64(vaddr(created)),
              thread_num,
              to_uint64(vaddr(owner)),
              to_uint64(created->kstack.base),
              to_uint64(created->kstack.base) + created->kstack.size);

    return created;
}

}  // namespace proc
