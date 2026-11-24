#pragma once
#include <cstddef>
#include <cstdint>

namespace trap {

// Register and control state saved across a trap (layout pinned with tvec.S).
//
// The user path of tvec.S saves x[0..30], u_epc, and u_status, then loads
// k_sp, k_tp, and next_call (not exercised yet: no user mode). Preemptive
// switches occur on the trap path; threads also switch voluntarily in
// sched::sleep() and once during boot handoff in kmain().
//
//   x[31]      offset 0    x1..x30 at 0..232, interrupted x31 at 240
//   u_epc      offset 248  saved/resumed sepc
//   u_status   offset 256  saved sstatus (SIE clear; sret restores SPIE -> SIE)
//   k_sp       offset 264  thread kernel stack top (mm/vm.h)
//   k_tp       offset 272  hart cpuid (rewritten on resume)
//   next_call  offset 280  usertrap entry; returns the trap_frame to resume
struct trap_frame {
    uint64_t x[31];
    uint64_t u_epc;
    uint64_t u_status;
    uint64_t k_sp;
    uint64_t k_tp;  // cpuid
    uint64_t next_call;
};

static_assert(offsetof(trap_frame, x) == 0);
static_assert(offsetof(trap_frame, u_epc) == 31 * 8);
static_assert(offsetof(trap_frame, u_status) == 32 * 8);
static_assert(offsetof(trap_frame, k_sp) == 33 * 8);
static_assert(offsetof(trap_frame, k_tp) == 34 * 8);
static_assert(offsetof(trap_frame, next_call) == 35 * 8);
static_assert(sizeof(trap_frame) == 36 * 8);

// Supervisor trap entry (tvec.S), installed into stvec in direct mode (4-byte
// aligned; page-aligned by linker.ld).
extern "C" void tvec();

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
