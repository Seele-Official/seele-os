# Supervisor trap entry, installed by idle() with `csrw stvec, trap::tvec`
# (direct mode, so the entry must be 4-byte aligned; the linker script
# page-aligns this section). Every trap -- exception or interrupt -- arrives
# here, and preemptive thread switches happen on this path when trap.cc calls
# sched::schedule() (voluntary blocking switches also occur via sched::sleep(),
# and the initial one-way boot handoff happens in kmain()).
#
# sscratch distinguishes the origin of the trap:
#
#   - user side (sscratch != 0): sscratch points to this hart's per-hart
#       trap::trap_scratch (trap.h):
#         0(scratch)   kernel_sp    current thread's kstack top
#         8(scratch)   kernel_tp    this hart's dense cpuid
#         16(scratch)  user_sp_tmp  temporary stash for user sp
#   - kernel side (sscratch == 0): trap taken while already in supervisor mode,
#       including any nested trap inside the kernel.
#
# Both paths build the same 272-byte stack-allocated trap::trap_frame (trap.h)
# on the current thread's kernel stack:
#
#   0..240(sp)  x1..x31 (ra, sp, gp, tp, t0..t2, s0..s1, a0..a7, s2..s11, t3..t6)
#   248(sp)     sepc
#   256(sp)     sstatus
#   264(sp)     reserved (8-byte padding for 16-byte stack alignment)
#
# trap_frame, trap_scratch, and the offsets here are one contract: update
# trap.h and this file together (trap.h pins every offset with static_assert).

.macro SAVE_GPRS_X6_X30 base
    sd x6,   40(\base)
    sd x7,   48(\base)
    sd x8,   56(\base)
    sd x9,   64(\base)
    sd x10,  72(\base)
    sd x11,  80(\base)
    sd x12,  88(\base)
    sd x13,  96(\base)
    sd x14, 104(\base)
    sd x15, 112(\base)
    sd x16, 120(\base)
    sd x17, 128(\base)
    sd x18, 136(\base)
    sd x19, 144(\base)
    sd x20, 152(\base)
    sd x21, 160(\base)
    sd x22, 168(\base)
    sd x23, 176(\base)
    sd x24, 184(\base)
    sd x25, 192(\base)
    sd x26, 200(\base)
    sd x27, 208(\base)
    sd x28, 216(\base)
    sd x29, 224(\base)
    sd x30, 232(\base)
.endm

.macro LOAD_GPRS_X5_X31 base
    ld x5,   32(\base)
    ld x6,   40(\base)
    ld x7,   48(\base)
    ld x8,   56(\base)
    ld x9,   64(\base)
    ld x10,  72(\base)
    ld x11,  80(\base)
    ld x12,  88(\base)
    ld x13,  96(\base)
    ld x14, 104(\base)
    ld x15, 112(\base)
    ld x16, 120(\base)
    ld x17, 128(\base)
    ld x18, 136(\base)
    ld x19, 144(\base)
    ld x20, 152(\base)
    ld x21, 160(\base)
    ld x22, 168(\base)
    ld x23, 176(\base)
    ld x24, 184(\base)
    ld x25, 192(\base)
    ld x26, 200(\base)
    ld x27, 208(\base)
    ld x28, 216(\base)
    ld x29, 224(\base)
    ld x30, 232(\base)
    ld x31, 240(\base)
.endm

.section .text.tvec,"ax",@progbits
.globl trampoline
trampoline:

.align 4
.globl tvec
.type tvec,@function
tvec:
    # Swap t6 (x31) with sscratch: a user trap yields &mycpu()->trap_scratch in
    # t6 (and stashes user t6 in sscratch); a kernel trap yields zero.
    csrrw t6, sscratch, t6
    beqz t6, kernel_trap

user_trap:
    # Stash user sp in trap_scratch.user_sp_tmp, switch to this thread's kernel
    # stack top, and reserve the 272-byte trap_frame.
    sd sp, 16(t6)
    ld sp, 0(t6)
    addi sp, sp, -272

    # Save user t0 first so t0 can serve as temporary while retrieving user t6
    # and user sp. Clearing sscratch atomically marks "now in kernel" so any
    # nested trap takes kernel_trap.
    sd t0, 32(sp)
    csrrw t0, sscratch, zero
    sd t0, 240(sp)
    ld t0, 16(t6)
    sd t0, 8(sp)

    # Save the remaining user GPRs (x1, x3, x4, x6..x30).
    sd x1,   0(sp)
    sd x3,  16(sp)
    sd x4,  24(sp)
    SAVE_GPRS_X6_X30 sp

    # Save interrupted sepc and sstatus.
    csrr t0, sepc
    sd t0, 248(sp)
    csrr t0, sstatus
    sd t0, 256(sp)

    # Restore kernel tp (this hart's cpuid). The kernel builds with
    # --no-relax-gp and does not rely on gp.
    ld tp, 8(t6)

    # Clear frame pointer so stacktrace::print() stops cleanly at the
    # user/kernel boundary instead of following a user-controlled s0.
    mv s0, zero

    # trap_scratch* usertrap(trap_frame* frame): the ABI guarantees sp still
    # points at trap_frame on return, and a0 receives the current hart's
    # trap_scratch* (which may differ from entry if this thread migrated).
    mv a0, sp
    call usertrap
    # Fall through directly into tvecret.

# User-mode return path. Contract:
#   - sstatus.SIE == 0
#   - sp == trap_frame* (at kstack_top - 272)
#   - a0 == trap_scratch* (&mycpu()->trap_scratch)
#
# Executable page loaders (not tvecret) are responsible for executing fence.i
# when writing user code pages.
.globl tvecret
.type tvecret,@function
tvecret:
    # Record kstack_top in this hart's trap_scratch for the next user trap.
    addi t0, sp, 272
    sd t0, 0(a0)

    # Restore sepc and sstatus. SIE is clear in saved sstatus, so interrupts
    # stay disabled through the restore sequence and re-enable atomically on
    # sret (SPIE -> SIE).
    ld t0, 248(sp)
    ld t1, 256(sp)
    csrw sepc, t0
    csrw sstatus, t1

    # Arm sscratch with this hart's trap_scratch* before restoring user GPRs.
    csrw sscratch, a0

    # Restore x1, x3..x31 while sp still points at trap_frame.
    ld x1,   0(sp)
    ld x3,  16(sp)
    ld x4,  24(sp)
    LOAD_GPRS_X5_X31 sp

    # Restore user sp (x2) last.
    ld x2, 8(sp)

    sret
    .size tvecret, .-tvecret

# First-time U-mode entry: void enter_user(trap_frame* frame, trap_scratch* scratch).
# Moves arguments into tvecret's register contract (sp = frame, a0 = scratch).
.align 2
.globl enter_user
.type enter_user,@function
enter_user:
    mv sp, a0
    mv a0, a1
    j tvecret
    .size enter_user, .-enter_user

kernel_trap:
    # t6 is zero here; swap back to restore interrupted t6 and keep
    # sscratch == 0.
    csrrw t6, sscratch, t6

    # Allocate the unified 272-byte trap_frame on the current kernel stack.
    addi sp, sp, -272

    # Save x1, x3..x31 into trap_frame.
    sd x1,   0(sp)
    sd x3,  16(sp)
    sd x4,  24(sp)
    sd x5,  32(sp)
    SAVE_GPRS_X6_X30 sp
    sd x31, 240(sp)

    # Record the exact pre-trap sp in frame->sp.
    addi t0, sp, 272
    sd t0, 8(sp)

    # Save interrupted sepc and sstatus into trap_frame.
    csrr t0, sepc
    sd t0, 248(sp)
    csrr t0, sstatus
    sd t0, 256(sp)

    # s0 is preserved untouched so stacktrace::print() can unwind through the
    # interrupted kernel frame.
    mv a0, sp
    call kerneltrap

    # Restore sepc and sstatus from trap_frame: this thread may have been
    # switched out during kerneltrap while other threads ran traps of their own.
    ld t0, 248(sp)
    ld t1, 256(sp)
    csrw sepc, t0
    csrw sstatus, t1

    # Restore x1, x3, x5..x31. Do NOT restore x2 (sp) or x4 (tp): tp holds this
    # hart's cpuid, which may differ if this thread migrated across harts.
    ld x1,   0(sp)
    ld x3,  16(sp)
    LOAD_GPRS_X5_X31 sp

    addi sp, sp, 272
    sret
