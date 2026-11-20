#include "boot/stack.h"

    # M-mode entry point. With QEMU `virt` and `-bios none` execution starts
    # here at 0x8000_0000 with a0 = hartid and a1 = FDT address.
    #
    # This is the only code in the kernel that runs without a stack, and it is
    # assembly on purpose: the compiler is free to emit a prologue for a C++
    # function (-fno-omit-frame-pointer makes it do so even for a leaf), so
    # "start() does not use the stack" is not something the build can promise.
    # Keeping the pre-stack work in .S makes the guarantee structural instead
    # of an optimization artifact.
    #
    # Hart 0 is the boot hart: it gets the large stack and later runs the whole
    # boot path. The other harts get a small stack and a dense cpuid from
    # boot_hart_seq, an atomic arrival-order counter in .data. The counter must
    # not live in .bss: hart 0 clears .bss later, and a secondary that starts
    # after that point must neither lose nor corrupt its number. Secondary
    # cpuid 0 is never handed out; it belongs to the boot hart. If the pool is
    # exhausted, park the hart instead of running it on somebody else's stack.
    .section .text.boot,"ax",@progbits
    .globl _entry
    .type _entry,@function
_entry:
    .option push
    .option norelax
    la gp, __global_pointer$
    .option pop

    bnez a0, .Lsecondary

    # Boot hart (hartid 0): cpuid 0, large stack.
    li tp, 0
    la sp, boot_stacks
    li t0, SEELE_BOOT_STACK_SIZE
    add sp, sp, t0

    # start() never returns. tail keeps the pc-relative jump in range even if
    # the kernel image grows beyond the +/-1 MiB reach of a plain `j`.
    tail start

.Lsecondary:
    # boot_hart_seq starts at 1, so the first secondary reads 1, the next 2,
    # and so on. tp is the dense per-hart index used by cpus[] and spinlocks;
    # hartid stays in a0 and is handed to kmain for FDT lookups.
    #
    # MAX_HARTS is both the number of cpus[] entries and the number of boot
    # stack slots, so one compare is enough: a hart whose cpuid would fall
    # outside either bound parks instead of running on somebody else's stack.
    la t0, boot_hart_seq
    li t1, 1
    amoadd.d t2, t1, (t0)       # t2 = old value = this hart's cpuid
    li t0, SEELE_MAX_HARTS
    bgeu t2, t0, .Lpark

    # sp = boot_stacks
    #      + SEELE_BOOT_STACK_SIZE
    #      + (cpuid - 1) * SEELE_SECONDARY_STACK_SIZE
    #      + SEELE_SECONDARY_STACK_SIZE          (top of this hart's stack)
    la sp, boot_stacks
    li t0, SEELE_BOOT_STACK_SIZE + SEELE_SECONDARY_STACK_SIZE
    add sp, sp, t0
    addi t1, t2, -1
    li t0, SEELE_SECONDARY_STACK_SIZE
    mul t1, t1, t0
    add sp, sp, t1

    mv tp, t2
    tail start

.Lpark:
    wfi
    j .Lpark

    .size _entry, .-_entry
