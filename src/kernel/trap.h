#pragma once
#include <cstddef>
#include <cstdint>

namespace trap {

// Per-hart minimal scratch area addressed via sscratch while running in U-mode
// (layout pinned with tvec.S).
//
// While executing in S-mode, sscratch is 0 so tvec.S recognizes kernel traps.
// Before sret to U-mode, tvecret updates kernel_sp to the resuming thread's
// kstack top and loads &mycpu()->trap_scratch into sscratch.
//
//   kernel_sp    offset 0   current thread's kstack top (updated in tvecret)
//   kernel_tp    offset 8   this hart's dense cpuid (initialized once per hart)
//   user_sp_tmp  offset 16  temporary stash for user sp on user trap entry
struct trap_scratch {
    uint64_t kernel_sp;
    uint64_t kernel_tp;
    uint64_t user_sp_tmp;
};

static_assert(offsetof(trap_scratch, kernel_sp) == 0);
static_assert(offsetof(trap_scratch, kernel_tp) == 8);
static_assert(offsetof(trap_scratch, user_sp_tmp) == 16);
static_assert(sizeof(trap_scratch) == 24);

// Unified stack-based trap frame saved on the current thread's kernel stack by
// both user and kernel paths of tvec.S (layout pinned with tvec.S).
//
// Fields follow RISC-V GPR order (x1..x31), followed by sepc, sstatus, and an
// 8-byte alignment pad so the 272-byte frame preserves the 16-byte psABI stack
// alignment.
struct trap_frame {
    uint64_t ra;        // offset 0   (x1)
    uint64_t sp;        // offset 8   (x2)
    uint64_t gp;        // offset 16  (x3)
    uint64_t tp;        // offset 24  (x4)
    uint64_t t0;        // offset 32  (x5)
    uint64_t t1;        // offset 40  (x6)
    uint64_t t2;        // offset 48  (x7)
    uint64_t s0;        // offset 56  (x8)
    uint64_t s1;        // offset 64  (x9)
    uint64_t a0;        // offset 72  (x10)
    uint64_t a1;        // offset 80  (x11)
    uint64_t a2;        // offset 88  (x12)
    uint64_t a3;        // offset 96  (x13)
    uint64_t a4;        // offset 104 (x14)
    uint64_t a5;        // offset 112 (x15)
    uint64_t a6;        // offset 120 (x16)
    uint64_t a7;        // offset 128 (x17)
    uint64_t s2;        // offset 136 (x18)
    uint64_t s3;        // offset 144 (x19)
    uint64_t s4;        // offset 152 (x20)
    uint64_t s5;        // offset 160 (x21)
    uint64_t s6;        // offset 168 (x22)
    uint64_t s7;        // offset 176 (x23)
    uint64_t s8;        // offset 184 (x24)
    uint64_t s9;        // offset 192 (x25)
    uint64_t s10;       // offset 200 (x26)
    uint64_t s11;       // offset 208 (x27)
    uint64_t t3;        // offset 216 (x28)
    uint64_t t4;        // offset 224 (x29)
    uint64_t t5;        // offset 232 (x30)
    uint64_t t6;        // offset 240 (x31)
    uint64_t sepc;      // offset 248
    uint64_t sstatus;   // offset 256
    uint64_t reserved;  // offset 264 (padding for 16-byte stack alignment)
};

static_assert(offsetof(trap_frame, ra) == 0);
static_assert(offsetof(trap_frame, sp) == 8);
static_assert(offsetof(trap_frame, t0) == 32);
static_assert(offsetof(trap_frame, a0) == 72);
static_assert(offsetof(trap_frame, t6) == 240);
static_assert(offsetof(trap_frame, sepc) == 248);
static_assert(offsetof(trap_frame, sstatus) == 256);
static_assert(offsetof(trap_frame, reserved) == 264);
static_assert(sizeof(trap_frame) == 34 * 8);
static_assert(sizeof(trap_frame) % 16 == 0);

// Supervisor trap entry (tvec.S), installed into stvec in direct mode (4-byte
// aligned; page-aligned by linker.ld).
extern "C" void tvec();

// Enter U-mode for the first time using an initial `frame` positioned at
// `kstack_top - sizeof(trap_frame)` and this hart's `scratch` area. Requires
// local interrupts disabled (sstatus.SIE == 0).
extern "C" [[noreturn]] void enter_user(trap_frame* frame, trap_scratch* scratch);

// External-interrupt routing: decouples the single platform interrupt
// controller (ExternalController) from per-source device drivers
// (ExternalHandler) via a small linear table (SEELE_MAX_IRQ_HANDLERS).
//
// Both objects live in driver private storage for kernel lifetime and are
// registered once on the boot hart before kmain()'s `g_started` release
// publishes them to secondaries (no runtime registration or locking). Handlers
// run on the trap path with interrupts disabled and must clear their pending
// device condition before returning.
class ExternalHandler {
public:
    virtual void handle(uint32_t irq) noexcept = 0;

protected:
    // Non-owning static/arena lifetime: forbid delete through this interface.
    ~ExternalHandler() = default;
};

class ExternalController {
public:
    // Claim, dispatch via dispatch_external_handler(), and complete pending
    // external interrupts; must not leave a claimed source uncompleted.
    virtual void handle() noexcept = 0;

    // Enable `irq` at `priority`; returns false if unsupported by the controller.
    [[nodiscard]] virtual bool enable(uint32_t irq, uint32_t priority) noexcept = 0;

protected:
    ~ExternalController() = default;
};

// Install the platform interrupt controller (must outlive the kernel).
void set_external_controller(ExternalController& controller) noexcept;

// Register `handler` for `irq` and enable it on the installed controller.
// Returns false (leaving the table untouched) for irq 0, duplicate irq, full
// table, missing controller, or controller rejection.
[[nodiscard]] bool register_external_handler(uint32_t irq,
                                             ExternalHandler& handler,
                                             uint32_t priority) noexcept;

// Dispatch a claimed source to its registered handler; unclaimed sources are
// ignored (the controller still completes them).
void dispatch_external_handler(uint32_t irq) noexcept;

}  // namespace trap
