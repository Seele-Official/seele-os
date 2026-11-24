# Supervisor trap entry, installed by idle() with `csrw stvec, trap::tvec`
# (direct mode, so the entry must be 4-byte aligned; the linker script
# page-aligns this section). Every trap -- exception or interrupt -- arrives
# here, and preemptive thread switches happen on this path when trap.cc calls
# sched::schedule() (voluntary blocking switches also occur via sched::sleep(),
# and the initial one-way boot handoff happens in kmain()).
#
# sscratch says which side the trap came from:
#
#   - user side:   sscratch holds the current thread's trap_frame pointer
#                  (trap.h). The first csrrw swaps it with x31, so x31 becomes
#                  the trapframe and sscratch keeps the user's x31.
#   - kernel side: sscratch is zero -- a trap taken while already in the
#                  kernel, including a trap inside the user-side handler.
#
# User path (sscratch != 0): store x1..x30 at trap_frame.x[0..29] and the old
# x31 at 240, copy sepc/sstatus into u_epc/u_status, then clear sscratch to
# mark "now in kernel" so a nested trap takes the kernel path. Load the frame's
# k_sp / k_tp / next_call (the thread's kernel stack slot, the hart's cpuid and the
# handler -- usertrap in trap.cc) and jump through next_call. The handler
# returns the trapframe to resume in a0; after scheduling it may be a different
# thread's frame, which is how a trap turns into a thread switch without
# touching any state but the frames.
#
# The frame's control part (k_sp, k_tp, next_call) is prepared by whoever sends
# a thread into user mode. Nothing does that yet -- no code enters user mode --
# so the user path below has never run.
#
# Kernel path (sscratch == 0): restore x31, reserve 256 bytes below sp, save
# the caller-saved registers at their trap_frame offsets (trap.h) and call
# kerneltrap, then restore and sret. sscratch stays zero throughout, so a trap
# taken inside kerneltrap re-enters this path.
#
# trap_frame and the offsets here are one contract: change trap.h and this
# file together (the struct pins its own layout with static_asserts).
.section .text.tvec,"ax",@progbits
.globl trampoline
trampoline:

.align 4
.globl tvec
.type tvec,@function
tvec:
    # Swap x31 with sscratch: a user-side trap yields the trapframe in x31,
    # a kernel-side trap yields zero (and stashes x31 in sscratch).
    csrrw x31, sscratch, x31
    # zero means the trap was taken from the kernel
    beqz x31, kernel

    sd x1, 0(x31)
    sd x2, 8(x31)
    sd x3, 16(x31)
    sd x4, 24(x31)
    sd x5, 32(x31)
    sd x6, 40(x31)
    sd x7, 48(x31)
    sd x8, 56(x31)
    sd x9, 64(x31)
    sd x10, 72(x31)
    sd x11, 80(x31)
    sd x12, 88(x31)
    sd x13, 96(x31)
    sd x14, 104(x31)
    sd x15, 112(x31)
    sd x16, 120(x31)
    sd x17, 128(x31)
    sd x18, 136(x31)
    sd x19, 144(x31)
    sd x20, 152(x31)
    sd x21, 160(x31)
    sd x22, 168(x31)
    sd x23, 176(x31)
    sd x24, 184(x31)
    sd x25, 192(x31)
    sd x26, 200(x31)
    sd x27, 208(x31)
    sd x28, 216(x31)
    sd x29, 224(x31)
    sd x30, 232(x31)

    # the user's x31 is the value the swap left in sscratch
    csrr t0, sscratch

    # clear sscratch: from here on we are in the kernel, so a nested trap
    # takes the kernel path
    csrw sscratch, x0

    sd t0, 240(x31)

    # trapframe.u_epc: the pc the user was interrupted at
    csrr t0, sepc
    sd t0, 248(x31)
    # trapframe.u_status: sstatus as the trap left it -- SPP clear, SPIE holding
    # the user's SIE, SIE clear -- so writing it back before sret keeps
    # interrupts off until the sret itself
    csrr t0, sstatus
    sd t0, 256(x31)

    # trapframe.k_sp: the current thread's kernel stack slot
    ld sp, 264(x31)
    # trapframe.k_tp: the hart's cpuid (the thread may have been scheduled
    # onto another hart since the frame was saved)
    ld tp, 272(x31)
    # trapframe.next_call: the handler to call (usertrap)
    ld t1, 280(x31)

    # the handler returns the trapframe to resume in a0; after scheduling it
    # may be a different thread's frame. ra names tvecret explicitly so the
    # handler's ret lands there even if the assembler pads before the label.
    mv a0, x31
    la ra, tvecret
    jr t1

.align 4
# The resume half of the user path. Exported for symbolization; the handler
# returns into tvecret.
.globl tvecret
.type tvecret,@function
tvecret:

    # any code the handler wrote must be visible before the resumed thread
    # fetches it
    fence.i

    # Store the frame back into sscratch: the next trap from user mode finds
    # its trapframe there. (From the clear at entry until this store the hart
    # ran in the kernel with sscratch zero; after it, only the restore loop
    # and sret remain, with interrupts still disabled from trap entry.)
    csrw sscratch, a0
    mv x31, a0

    # trapframe.k_tp: record this hart in the frame being resumed. The frame
    # may belong to another thread, and its next trap on this hart must find
    # this hart's cpuid there.
    sd tp, 272(x31)

    # trapframe.u_epc: the pc to resume, after any change the handler made
    ld t0, 248(x31)
    # trapframe.u_status: SIE is clear in the saved value, so interrupts stay
    # off through the register restore below and are enabled only by sret
    ld t1, 256(x31)

    csrw sepc, t0
    csrw sstatus, t1

    ld x1, 0(x31)
    ld x2, 8(x31)
    ld x3, 16(x31)
    ld x4, 24(x31)
    ld x5, 32(x31)
    ld x6, 40(x31)
    ld x7, 48(x31)
    ld x8, 56(x31)
    ld x9, 64(x31)
    ld x10, 72(x31)
    ld x11, 80(x31)
    ld x12, 88(x31)
    ld x13, 96(x31)
    ld x14, 104(x31)
    ld x15, 112(x31)
    ld x16, 120(x31)
    ld x17, 128(x31)
    ld x18, 136(x31)
    ld x19, 144(x31)
    ld x20, 152(x31)
    ld x21, 160(x31)
    ld x22, 168(x31)
    ld x23, 176(x31)
    ld x24, 184(x31)
    ld x25, 192(x31)
    ld x26, 200(x31)
    ld x27, 208(x31)
    ld x28, 216(x31)
    ld x29, 224(x31)
    ld x30, 232(x31)
    ld x31, 240(x31)

    sret
kernel:
    # x31 is zero here; swap back so sscratch returns to zero and x31 is
    # restored to its pre-trap value.
    csrrw x31, sscratch, x31
    # make room to save registers.
    addi sp, sp, -256

    # save caller-saved registers at their trap_frame.x[] offsets (trap.h)
    sd ra, 0(sp)
    # sd sp, 8(sp)
    sd gp, 16(sp)
    # sd tp, 24(sp)
    sd t0, 32(sp)
    sd t1, 40(sp)
    sd t2, 48(sp)
    sd a0, 72(sp)
    sd a1, 80(sp)
    sd a2, 88(sp)
    sd a3, 96(sp)
    sd a4, 104(sp)
    sd a5, 112(sp)
    sd a6, 120(sp)
    sd a7, 128(sp)
    sd t3, 216(sp)
    sd t4, 224(sp)
    sd t5, 232(sp)
    sd t6, 240(sp)

    # call the C trap handler in trap.cc
    call kerneltrap

    # restore registers.
    ld ra, 0(sp)
    # ld sp, 8(sp)
    ld gp, 16(sp)
    # not tp (contains hartid), in case we moved CPUs
    ld t0, 32(sp)
    ld t1, 40(sp)
    ld t2, 48(sp)
    ld a0, 72(sp)
    ld a1, 80(sp)
    ld a2, 88(sp)
    ld a3, 96(sp)
    ld a4, 104(sp)
    ld a5, 112(sp)
    ld a6, 120(sp)
    ld a7, 128(sp)
    ld t3, 216(sp)
    ld t4, 224(sp)
    ld t5, 232(sp)
    ld t6, 240(sp)

    addi sp, sp, 256

    # return to whatever we were doing in the kernel.
    sret
