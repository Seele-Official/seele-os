#pragma once
#include <cstddef>
#include <cstdint>

namespace trap {

// The register state a trap saves: everything the interrupted code owned, plus
// the kernel-side state the handler needs to run and to resume.
//
// tvec.S writes and reads these fields at fixed offsets, so the layout is a
// contract with the assembly (pinned by the static_asserts below). The user
// path of tvec.S fills the x[] / u_epc / u_status half of the frame itself; the
// k_sp / k_tp / next_call half is the frame's "control" part, prepared by
// whoever sends a thread into user mode. No C++ code prepares a frame yet --
// nothing enters user mode -- so the user path has not run:
//
//   x[31]      offset 0    x1..x30 at 0..232, the interrupted x31 at 240
//   u_epc      offset 248  the interrupted pc: copied from sepc on entry and
//                          written back to sepc by the return path, possibly
//                          after the handler changed it
//   u_status   offset 256  sstatus as the trap left it: SPP clear, SPIE holds
//                          the user's SIE and SIE is clear, so restoring it
//                          before sret keeps interrupts off until the sret
//   k_sp       offset 264  the kernel stack the trap enters on: the current
//                          thread's kstack slot top (mm/vm.h), loaded into sp
//   k_tp       offset 272  the hart's cpuid, loaded on entry because the
//                          thread may have been scheduled onto another hart;
//                          the return path rewrites it with the hart that is
//                          actually resuming the frame
//   next_call  offset 280  the handler to call from the user path (usertrap).
//                          It receives the frame in a0 and returns the frame to
//                          resume; after the trap path has scheduled
//                          (shed::switch_thread, shed/switch.h) that may be a
//                          different thread's frame.
//
// This frame is where scheduling hangs its hat: the trap path is the only
// place the kernel switches threads, and the frame the return path restores is
// whichever thread the scheduler picked.
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

// The supervisor trap entry (tvec.S): installed by idle() into stvec (direct
// mode), so it must be 4-byte aligned; the linker script page-aligns it.
extern "C" void tvec();

// External-interrupt routing: a linear table of per-source handler objects plus
// the controller object that delivers them.
//
// An external interrupt (scause == EXT_INT) means the platform interrupt
// controller has at least one pending source. Two sides meet here:
//
//   - the interrupt controller driver (drivers/irq) owns claim/complete and
//     source enabling. The core discovers ExternalController on the object
//     returned by its Core probe and installs it with set_external_controller();
//     the trap path calls it on EXT_INT.
//   - a device driver owns one interrupt source. It registers itself in the
//     table with register_external_handler(); once the controller has claimed
//     a source, it calls dispatch_external_handler(irq) and the table entry for
//     that source runs.
//
// trap.h is the meeting point on purpose: a device driver asks for "interrupt
// 10" without knowing whether a PLIC, an APLIC or something else backs it, and
// the controller never includes a device driver's header.
//
// Both sides are objects because both carry state: the controller owns its
// register window and context, a handler owns its device state and locks. trap
// stores a borrowed pointer to each object instead of a function pointer plus a
// `void*` through which every caller would have to recover `this`. The objects
// live in the driver's private state for the lifetime of the kernel
// (drivers/registry.h), so neither trap nor the driver copies or destroys
// them. The table itself is linear and small (SEELE_MAX_IRQ_HANDLERS entries):
// registration happens once per driver during probe, and dispatch is a short
// walk on an interrupt, not a hot path that needs hashing.
//
// Handlers run on the trap path with interrupts disabled -- trap entry clears
// sstatus.SIE, and this trap path deliberately leaves them off (see kerneltrap
// in trap.cc). A handler must clear the condition it services before returning;
// a source it leaves pending stays pending.
//
// The table is keyed by the controller's source number and assumes one
// installed controller: the kernel has one platform interrupt controller today,
// so the numbers are unique machine-wide. A board with several controllers
// would need a controller id in the key as well.
//
// Installation and registration both happen during boot probe on the boot hart,
// before the secondary harts are released (kmain's `started` flag), so the
// table is published by that release/acquire pair and needs no lock; nothing
// registers at run time.
class ExternalHandler {
public:
    // Service the source this object registered for. `irq` is the source the
    // controller claimed, for a driver that owns more than one line; a driver
    // whose object owns a single source can ignore it. Runs on the trap path
    // with interrupts disabled and must clear the device's interrupt condition.
    virtual void handle(uint32_t irq) noexcept = 0;

protected:
    // A handler lives in static storage and is never destroyed: `delete`
    // through this interface must not compile. Same reasoning as CharDevice's
    // protected non-virtual destructor (char_device.h).
    ~ExternalHandler() = default;
};

// The controller half of the contract, implemented by the platform interrupt
// controller driver.
class ExternalController {
public:
    // Service one EXT_INT: claim a source, dispatch it through the table below
    // and complete it. The controller decides how many claims to service per
    // trap; it must not return with a claimed source left uncompleted.
    virtual void handle() noexcept = 0;

    // Route `irq` to the handler just registered and program its priority.
    // False when `irq` is not a source of this controller or cannot be enabled.
    [[nodiscard]] virtual bool enable(uint32_t irq, uint32_t priority) noexcept = 0;

protected:
    // The controller lives in static storage too and is never destroyed.
    ~ExternalController() = default;
};

// Install the controller. Called by the driver core after a successful probe;
// the reference must outlive the kernel.
void set_external_controller(ExternalController& controller) noexcept;

// Register `handler` for `irq` and ask the installed controller to enable the
// source. Returns false for interrupt number 0, a duplicate registration, a
// full table, no installed controller, or a controller that refuses the source.
// On every failure the table is left untouched, so the caller can fall back
// (the UART keeps polling).
[[nodiscard]] bool register_external_handler(uint32_t irq,
                                             ExternalHandler& handler,
                                             uint32_t priority) noexcept;

// Called by the controller for a source it has claimed. Walks the linear table
// and runs the matching object; a claimed source with no entry is dropped here
// (the controller completes it regardless, so it cannot wedge the gateway).
void dispatch_external_handler(uint32_t irq) noexcept;

}  // namespace trap
