#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string.h>

#include "console.h"
#include "hart.h"
#include "panic.h"
#include "riscv.h"
#include "trap.h"
#include "boot/info.h"
#include "boot/stack.h"
#include "drivers/registry.h"
#include "early/firmware.h"
#include "fdt/fdt.h"
#include "mm/kalloc.h"
#include "mm/vm.h"
#include "platform/device.h"
#include "proc/process.h"
#include "proc/shell.h"
#include "shed/switch.h"

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

// Secondaries read this while hart 0 is still booting, before clear_bss() has
// run. constinit only guarantees constant initialization; it does not choose a
// section, and a zero-valued constinit object still lands in .bss/.sbss with
// this toolchain. The section attribute is what puts the flag in the loaded
// segment, so its initial value is well defined even if the loader does not
// zero .bss. On QEMU the loader does zero it, so this is belt and suspenders,
// not a fix for an observed failure.
constinit static std::atomic<bool> started __attribute__((section(".data"))) = false;

// The tree the rest of the kernel sees. Static rather than a local: `Device`
// keeps node handles into it and `Device::name()` points into the blob, so the
// tree must outlive every use. It is assigned after clear_bss(), which is why
// it cannot be a constinit object.
static fdt::Tree tree;

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
    csrs(sstatus, csr::sstatus::SIE);
    while(true) {
        riscv::wfi();
    }
}

extern "C" [[noreturn]] void kmain(uint64_t hartid, Paddr dtb) {
    mycpu()->hartid = hartid;

    // cpuid was assigned by _entry, before any C++ ran: 0 is the boot hart,
    // the secondaries took the next values from the arrival-order counter.
    // Do not overwrite tp with hartid here -- hartid stays a plain argument
    // because firmware is free to hand out sparse or large ids.
    if(cpuid() == 0) {
        cxx_init();

        // The tree reads the blob through its direct-map alias: the firmware
        // hands over a physical address, and only P2V() turns one into memory
        // the kernel can dereference.
        tree = fdt::Tree{to_pointer<const std::byte>(vm::k::P2V(dtb))};

        // Attach a polling console before discovery can emit diagnostics.
        // Failure is nonfatal: output is dropped until a later driver attaches
        // a console, if one can be probed.
        (void)early::attach_console(tree);

        SEELE_ASSERT(tree.valid(), "fdt: invalid device tree, no devices discovered");
        // Enumerate before parsing, so a discovery warning reaches the console
        // that is already attached (when there is one; a missing sink just
        // drops the line, it never fails the boot).
        device::enumerate(tree);

        LOG_INFO("boot: hart {} cpuid {} dtb {:#018x}", hartid, cpuid(), to_uint64(dtb));
        LOG_DEBUG("boot: sstatus={:#018x}", riscv::csrr(sstatus));

        boot::info info = boot::parse(tree, dtb);

        // The FDT describes the harts but not which one the firmware booted;
        // the hart running kmain is the boot hart by definition.
        for(size_t i = 0; i < info.hart_count; ++i) {
            if(info.harts[i].hartid == hartid) {
                info.harts[i].boot = true;
                break;
            }
        }

        LOG_DEBUG("boot: timebase={:#x}", info.timebase_frequency);

        // Build the physical page allocator's free list from boot::info. The
        // DTB is one of the Reserved regions, so the pages the strings above
        // point into never leave the pool.
        phymem::init(std::span{info.regions, info.region_count});
        LOG_INFO("phymem: initialized");

        // Build the shared kernel mappings before creating the system process's
        // root. Every process root reuses these direct-map and heap subtrees.
        vm::k::init(std::span{info.regions, info.region_count}, device::all());

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
        // process. Queue it only when a console is attached; otherwise this
        // hart can still enter idle without an interactive shell.
        if(console::attached()) {
            mycpu()->ready.push_back(proc::make_thread(mycpu()->system, &proc::shell));
        }

        started.store(true, std::memory_order_release);
    } else {
        // Acquire the shared mappings before creating this hart's system
        // process. Its kernel threads use a private stack subtree while sharing
        // the kernel image, direct map and heap with the other harts.
        while(!started.load(std::memory_order_acquire)) {}

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
    shed::switch_thread(&old, mycpu()->idle);

    // Reaching here means something switched back to the throwaway boot
    // context. Its stack pages are freed by idle(), so this path is a bug,
    // not a resumable boot path.
    SEELE_UNREACHABLE();
}
