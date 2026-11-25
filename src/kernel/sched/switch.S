# Switch kernel threads: save the current callee-saved state into `from` and
# resume `to` (declared in sched/switch.h, sched::switch_thread()).
#
# a0 = proc::thread* from   (receives ra, sp, s0-s11)
# a1 = proc::thread* to     (the thread to resume)
#
# A thread is the scheduling unit, and its layout is pinned by the
# static_asserts in proc/process.h. The process pointer sits first, then the
# kernel_context (offsetof(thread, context) == sizeof(process*) == 8), so every
# context field is read from a0/a1 plus 8:
#
#   0(a0)   proc      (the process the thread runs in)
#   8(a0)   context.ra
#   16(a0)  context.sp
#   24(a0)  context.s0
#   ...
#   112(a0) context.s11
#
# Only what the C ABI calls callee-saved is part of a thread context. a0 and a1
# are caller-saved, so a resumed call sees them clobbered exactly as the ABI
# allows. The kernel does not use gp (--no-relax-gp), and tp belongs to the
# hart, so neither is saved or restored: the hart keeps its cpuid across a
# switch.
#
# The address space follows the thread. When the two threads belong to
# different processes, the page table must change: load to->proc->satp
# (process.satp sits at offset 0 of the struct, pinned in proc/process.h) and
# flush the TLB around the satp write, so no stale translation survives the
# switch. Threads of the same process share one page table, so that case skips
# the write entirely -- and keeps the TLB warm.
#
# switch_thread is called from the trap path after a trap has been handled, and
# from sched::sleep() when a thread parks itself (console::read() does). Both
# callers have interrupts disabled and hold no lock. Every access here is
# a0/a1-relative, so nothing reads through the old sp once it changes.
#
# This is a real two-way switch, not a one-shot jump: when `from` is scheduled
# again, the resumed call returns to the C++ caller and execution continues
# after switch_thread(). kmain.cc's initial launch passes a throwaway `from`
# and never switches back to it.
    .text
    .globl switch_thread
    .type switch_thread,@function
switch_thread:
    sd ra, 8(a0)
    sd sp, 16(a0)
    sd s0, 24(a0)
    sd s1, 32(a0)
    sd s2, 40(a0)
    sd s3, 48(a0)
    sd s4, 56(a0)
    sd s5, 64(a0)
    sd s6, 72(a0)
    sd s7, 80(a0)
    sd s8, 88(a0)
    sd s9, 96(a0)
    sd s10, 104(a0)
    sd s11, 112(a0)

    # thread.proc. Same process: keep the current satp and TLB.
    ld t0, 0(a0)
    ld t1, 0(a1)

    beq t0, t1, load

    # Different process: switch the address space to to->proc->satp. The
    # fence before the write retires translations made under the old table;
    # the one after publishes the new root and makes future ones see it.
    ld t0, (t1)
    sfence.vma zero, zero
    csrw satp, t0
    sfence.vma zero, zero
load:
    ld ra, 8(a1)
    ld sp, 16(a1)
    ld s0, 24(a1)
    ld s1, 32(a1)
    ld s2, 40(a1)
    ld s3, 48(a1)
    ld s4, 56(a1)
    ld s5, 64(a1)
    ld s6, 72(a1)
    ld s7, 80(a1)
    ld s8, 88(a1)
    ld s9, 96(a1)
    ld s10, 104(a1)
    ld s11, 112(a1)
    ret
    .size switch_thread, .-switch_thread
