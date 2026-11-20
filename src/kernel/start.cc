// M-mode bootstrap.
//
// _entry (start.S) runs without a stack, picks the per-hart boot stack and the
// dense cpuid, and only then reaches start(), so this file is ordinary C++ and
// is allowed to use the stack. Everything else M-mode has to do before the
// kernel proper starts lives here: enable the FPU, open up PMP for S-mode,
// delegate traps to S-mode, and finally mret into S-mode.
//
// kmain is the S-mode entry point; its implementation is not part of this
// file.
#include <cstddef>
#include <cstdint>
#include <utility>

#include "hart.h"
#include "riscv.h"
#include "boot/stack.h"
#include "mm/addr.h"

// S-mode entry point, implemented in kmain.cc. `dtb` is the physical address of
// the blob the firmware passed in a1; it is typed here so it cannot be swapped
// with `hartid`, the other same-sized word of the handoff.
extern "C" [[noreturn]] void kmain(uint64_t hartid, Paddr dtb);

namespace {

using namespace riscv;
using namespace riscv::registers;

// One secondary boot stack: a page-aligned run of SECONDARY_STACK_SIZE bytes.
// Naming it keeps the boot_stack_pool member declaration readable (and inside the
// column limit) instead of spelling a two-dimensional array.
using SecondaryStack = std::byte[boot::SECONDARY_STACK_SIZE];

// Leave M-mode for S-mode.
//
// mret restores pc from mepc and the privilege mode from mstatus.MPP, and does
// not touch any GPR. That is what lets hartid and dtb cross the boundary in
// a0/a1, exactly where the lp64 ABI expects the first two arguments of
// s_mode_entry(). Two details make the handoff safe:
//
//  - a0/a1 are listed as clobbers. Without that, GCC could put hartid in a1 and
//    dtb in a0; the first move would then overwrite dtb before the second move
//    copied it to a1. Keeping the inputs out of a0/a1 makes the two moves
//    alias-free.
//  - the block ends in mret and is followed by std::unreachable(), so the
//    compiler emits no epilogue or fall-through code that would run after the
//    privilege switch. Both this block and the mstatus writes above are
//    volatile asm with a memory clobber, so they stay in program order.
[[noreturn]] void enter_s_mode(uint64_t hartid, Paddr dtb) noexcept {
    // mstatus.MPP = S.
    csrc(mstatus, csr::mstatus::MPP_MASK);
    csrs(mstatus, csr::mstatus::MPP_S);

    asm volatile("mv a0, %0\n"
                 "mv a1, %1\n"
                 "csrw mepc, %2\n"
                 "mret\n"
                 :
                 : "r"(hartid), "r"(to_uint64(dtb)), "r"(&kmain)
                 : "a0", "a1", "memory");
    std::unreachable();
}

}  // namespace

extern "C" {
    // The boot stack pool, laid out exactly as boot::stack_offset() and
    // boot::stack_size() describe it: one large stack for hart 0 followed by
    // one small stack per possible secondary cpuid (1..STACK_COUNT-1).
    //
    // A single object, rather than separate arrays, is what guarantees the
    // layout: with -fdata-sections every global gets its own input section,
    // and the linker is free to order those sections. Keeping the stacks as
    // members of one object makes their offsets a property of the type, which
    // is what _entry hard-codes in its address arithmetic.
    struct boot_stack_pool {
        alignas(riscv::PGSIZE) std::byte boot[boot::BOOT_STACK_SIZE];
        // One slice per possible secondary cpuid; STACK_COUNT is MAX_HARTS, and
        // cpuid 0 belongs to the boot hart above.
        alignas(riscv::PGSIZE) SecondaryStack secondary[boot::STACK_COUNT - 1];
    };

    // Its own input section so linker.ld can put it in the page-aligned .stack
    // output section after _end. The output section is NOLOAD: a stack does not
    // need initial contents, so it takes no file space.
    boot_stack_pool boot_stacks __attribute__((section(".stack")));

    // Dense cpuid assignment for secondary harts. _entry bumps it before the
    // boot hart has cleared .bss, so the initializer is deliberately non-zero:
    // a zero-initialized object would be emitted into .bss and the first
    // secondary would read garbage. hart 0 does not touch it and always gets
    // cpuid 0.
    uint64_t boot_hart_seq = 1;
}

static_assert(boot::stack_offset(0) == 0);
static_assert(offsetof(boot_stack_pool, boot) == boot::stack_offset(0));
static_assert(offsetof(boot_stack_pool, secondary) == boot::stack_offset(1));
static_assert(sizeof(boot_stack_pool) ==
              boot::stack_offset(boot::STACK_COUNT - 1) + boot::stack_size(boot::STACK_COUNT - 1));
static_assert(boot::STACK_COUNT == MAX_HARTS,
              "the boot stack pool must cover every cpuid cpus[] is indexed by");
static_assert(boot::STACK_COUNT >= 2,
              "the pool needs one boot stack plus at least one secondary slot");
static_assert(SEELE_BOOT_STACK_SIZE > SEELE_SECONDARY_STACK_SIZE,
              "the boot hart's stack must be larger than a secondary's");
static_assert(SEELE_BOOT_STACK_SIZE % riscv::PGSIZE == 0 &&
                  SEELE_SECONDARY_STACK_SIZE % riscv::PGSIZE == 0,
              "each boot stack must be a whole number of pages");

extern "C" [[noreturn]] void start(uint64_t hartid, Paddr dtb) noexcept {
    // mstatus.FS = Initial: without this any F/D instruction traps, even though
    // the hart implements them. FP context save/restore is not implemented yet.
    // FS is a two-bit field, so it is cleared before being set: setting bit 13
    // on top of an existing Dirty (0b11) would leave it Dirty.
    csrc(mstatus, csr::mstatus::FS_MASK);
    csrs(mstatus, csr::mstatus::FS_INITIAL);

    // S-mode starts without paging: the shared kernel page table needs phymem
    // and the FDT, so kmain() builds it and loads it into satp later
    // (vm::k::init/enable).
    csrw(satp, 0);

    // pmpaddr0 holds address >> 2, so all ones means "up to the top of the
    // address space"; TOR then covers [0, 2^64). QEMU `virt` starts us in
    // M-mode with no PMP entry configured, and S-mode cannot touch RAM or MMIO
    // until one exists.
    csrw(pmpaddr0, ~0ull);
    csrw(pmpcfg0, csr::pmpcfg::RWX_TOR);

    // Let S-mode handle the exceptions and interrupts it is meant to own. Both
    // CSRs are WARL, so bits for traps the hart does not implement are ignored.
    csrw(medeleg, 0xffffull);
    csrw(mideleg, 0xffffull);

    // enable hardware updating of PTE A/D bits in S-mode
    csrs(menvcfg, csr::menvcfg::AUDE);

    // enable Sstc extension
    csrs(menvcfg, csr::menvcfg::STCE);
    // mcounteren is clear at reset, which makes every S-mode read of cycle,
    // time, instret and the HPM counters trap. Let S-mode see all of them; the
    // bits of counters the hart does not implement are WARL and ignored.
    csrw(mcounteren, csr::mcounteren::ALL);

    enter_s_mode(hartid, dtb);
}
