#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string.h>

#include "console.h"
#include "hart.h"
#include "panic.h"
#include "riscv.h"
#include "trap.h"
#include "boot/stack.h"
#include "drivers/registry.h"
#include "mm/kalloc.h"
#include "mm/vm.h"
#include "platform/device.h"
#include "platform/platform.h"
#include "proc/process.h"
#include "proc/shell.h"
#include "sched/switch.h"

using namespace riscv;
using namespace riscv::registers;

void cxx_init() {
    // clear .bss
    memset(vm::BSS.start, 0, vm::BSS.size());

    // init cxx obj
    using Constructor = void (*)();
    for(auto fn = reinterpret_cast<Constructor*>(vm::INIT_ARRAY.start);
        fn != reinterpret_cast<Constructor*>(vm::INIT_ARRAY.end);
        ++fn) {
        (*fn)();
    }
}

// Secondaries read this while hart 0 is still booting, before cxx_init() has
// run. constinit only guarantees constant initialization; it does not choose a
// section, and a zero-valued constinit object still lands in .bss/.sbss with
// this toolchain. The section attribute is what puts the flag in the loaded
// segment, so its initial value is well defined even if the loader does not
// zero .bss. On QEMU the loader does zero it, so this is belt and suspenders,
// not a fix for an observed failure.
static constinit std::atomic<bool> g_started __attribute__((section(".data"))) = false;

// Called from idle() only after switching to that thread's kernel stack; the
// abandoned boot context must never resume after these pages are released.
static void free_boot_stack() noexcept {
    const Paddr base = vm::k::V2P(vaddr(vm::BOOT_STACK.start + boot::stack_offset(cpuid())));
    const size_t size = boot::stack_size(cpuid());
    for(size_t offset = 0; offset < size; offset += riscv::PGSIZE) {
        phymem::free(base + offset);
    }
}

// First thread entered on each hart. It finishes the boot-to-thread handoff
// and waits for interrupts when no other kernel thread is ready; idle itself
// is not a participant in the ready queue.
[[noreturn]] static void idle() noexcept {
    free_boot_stack();

    // The frame pointer is inside the idle thread's own kernel stack slot; the
    // address is the per-process high window, never the boot stack. Logging it
    // makes the switch_thread() stack switch visible in a boot log.
    LOG_DEBUG("proc: hart {} idle entered, frame {:#018x}",
              cpuid(),
              to_uint64(vaddr(__builtin_frame_address(0))));

    csrw(stimecmp, csrr(time) + 1000000);
    csrw(stvec, reinterpret_cast<uint64_t>(trap::tvec));

    // Trap entry and the first timer deadline must be ready before interrupts
    // can run the scheduler. A ready kernel thread replaces idle; idle resumes
    // only when no other thread is runnable.
    csrs(sie, csr::sie::SEIE | csr::sie::STIE);
    intr_on();
    while(true) {
        riscv::wfi();
    }
}

extern "C" [[noreturn]] void kmain(uint64_t hartid, Paddr dtb) {

    // cpuid was assigned by _entry, before any C++ ran: 0 is the boot hart,
    // the secondaries took the next values from the arrival-order counter.
    // Do not overwrite tp with hartid here -- hartid stays a plain argument
    // because firmware is free to hand out sparse or large ids.
    if(cpuid() == 0) {
        cxx_init();
        mycpu()->hartid = hartid;

        // Bind the firmware DTB, bring up the early console if available,
        // enumerate devices, and discover memory/hart topology.
        const platform::Info& plat = platform::init(dtb, hartid);

        LOG_INFO("boot: hart {} cpuid {} dtb {:#018x}", hartid, cpuid(), to_uint64(dtb));
        LOG_DEBUG("boot: sstatus={:#018x}", riscv::csrr(sstatus));
        LOG_DEBUG("boot: timebase={:#x}", plat.timebase_frequency());

        // Build the physical page allocator's free list from platform::MemoryMap.
        // The DTB is recorded in reserved(), so every borrowed string_view stays
        // backed for the kernel's lifetime.
        phymem::init(plat.memory());
        LOG_INFO("phymem: initialized");

        // Build the shared kernel mappings before creating the system process's
        // root. Every process root reuses these direct-map and heap subtrees.
        vm::k::init(plat.memory(), device::all());

        // Build the process's own root page table before paging is on. It copies
        // the kernel root entries that vm::k::init() froze, so the new table sees
        // the same image, direct map and heap subtrees as the shared kernel table;
        // its private kernel stack subtree is reserved but left empty until the
        // threads that need stacks map their slots.
        const Paddr pagetable = vm::k::create_process_pagetable();

        // Enable the heap window before allocating the system process and its
        // kernel threads; their objects live outside the direct map.
        vm::k::enable(pagetable);

        // All kernel threads created on this hart belong to its system process.
        // For now each hart has its own because scheduling does not cross harts.
        // Idle is the first thread, providing a fallback when no work is ready.
        mycpu()->system = proc::make_proc(pagetable);
        mycpu()->idle = proc::make_thread(mycpu()->system, &idle);

        LOG_DEBUG("boot: satp={:#018x}", riscv::csrr(satp));

        // Probe controllers before devices so device IRQ registration can use
        // the installed external-interrupt controller.
        driver::probe_all(device::all());

        // Shell is another kernel thread in the system process, not a separate
        // process. Queue it only when a readable console device is attached; an
        // output-only early sink leaves this hart running idle silently.
        if(console::can_read()) {
            mycpu()->ready.push_back(proc::make_thread(mycpu()->system, &proc::shell));
        }

        g_started.store(true, std::memory_order_release);
    } else {
        // Acquire the shared mappings before creating this hart's system
        // process. Its kernel threads use a private stack subtree while sharing
        // the kernel image, direct map and heap with the other harts.
        while(!g_started.load(std::memory_order_acquire)) {}
        mycpu()->hartid = hartid;
        const Paddr pagetable = vm::k::create_process_pagetable();

        vm::k::enable(pagetable);

        mycpu()->system = proc::make_proc(pagetable);
        mycpu()->idle = proc::make_thread(mycpu()->system, &idle);
    }

    SEELE_ASSERT(mycpu()->idle != nullptr, "proc: no idle thread to enter");

    // The idle thread is the first thread this hart runs. Record it before the
    // switch so mycpu()->current names the running thread from the moment the
    // hart leaves the boot context.
    mycpu()->current = mycpu()->idle;

    // The boot context is disposable, not a thread of the system process.
    // Its null process pointer forces switch_thread() to load the target satp,
    // which is already active. Do not enqueue or resume `old`.
    proc::thread old{};
    sched::switch_thread(&old, mycpu()->idle);

    // Reaching here means something switched back to the throwaway boot
    // context. Its stack pages are freed by idle(), so this path is a bug,
    // not a resumable boot path.
    SEELE_UNREACHABLE();
}
