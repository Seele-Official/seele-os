#include "trap.h"

#include <cstdint>

#include "console.h"
#include "hart.h"
#include "panic.h"
#include "riscv.h"
#include "shed/schedule.h"

using namespace riscv;
using namespace riscv::registers;

namespace trap {

// Capacity of the external-interrupt handler table. A build parameter
// (-DSEELE_MAX_IRQ_HANDLERS) like the other capacities; see trap.h for why the
// table is linear.
#ifndef SEELE_MAX_IRQ_HANDLERS
#error "SEELE_MAX_IRQ_HANDLERS is not defined: build the kernel through CMake (see CMakeLists.txt)"
#endif
constexpr uint32_t MAX_EXTERNAL_HANDLERS = SEELE_MAX_IRQ_HANDLERS;

namespace {

// One entry of the linear table: a source and the driver that owns it. The
// table and the controller pointer are written during boot probe on the boot
// hart, before the secondaries are released, so reads on other harts are
// published by kmain's `started` release/acquire pair and need no lock or
// atomics.
struct registration {
    uint32_t irq = 0;
    ExternalHandler* handler = nullptr;
};

// Both pointers are borrowed: the objects they name live in the drivers'
// private state for the lifetime of the kernel (see trap.h).
ExternalController* g_controller = nullptr;
registration g_handlers[MAX_EXTERNAL_HANDLERS]{};
uint32_t g_handler_count = 0;

// Route EXT_INT to the installed controller. A platform with no controller
// driver (or before its Core probe has run) simply drops the interrupt: the CSR
// side has nothing left to acknowledge once the trap has been taken.
void service_external() noexcept {
    if(g_controller != nullptr) {
        g_controller->handle();
    }
}

// Sources that were claimed without a registered handler. That is a firmware or
// driver bug, not a normal condition: the controller completes the claim
// anyway, so the source asserts again and the trap repeats. Warn once per
// source instead of once per claim -- a line per claim would flood the console
// and feed the UART's own transmit interrupt -- and keep the table small so a
// broken firmware cannot grow kernel state.
constexpr uint32_t MAX_WARNED_SOURCES = 8;
uint32_t g_warned_sources[MAX_WARNED_SOURCES]{};
uint32_t g_warned_count = 0;

void warn_unhandled_source(uint32_t irq) noexcept {
    for(uint32_t i = 0; i < g_warned_count; ++i) {
        if(g_warned_sources[i] == irq) {
            return;
        }
    }
    if(g_warned_count == MAX_WARNED_SOURCES) {
        return;
    }
    g_warned_sources[g_warned_count++] = irq;
    LOG_WARN("trap: claimed external source {} has no handler", irq);
}

}  // namespace

// The C++ half of the trap path (the entry is tvec.S). Scheduling happens here:
// after a trap has been handled and before the return, the handler may switch
// to the next thread (shed::switch_thread, shed/switch.h), so this is where the
// kernel preempts a thread or resumes one an interrupt woke. (A thread can also
// give the hart away on purpose, in shed::sleep().) Both interrupt causes the
// kernel handles are scheduling points: the timer preempts the running thread
// (shed::schedule), and an external interrupt may have woken one -- the console's
// receive interrupt wakes a reader parked in console::read() -- so it schedules
// too. Exceptions never schedule.

// Traps that arrived from user mode: tvec.S's sscratch path, which saved the
// interrupted registers (x1..x31, sepc, sstatus) in the trap_frame and switched
// to the kernel stack. The handler receives that frame in a0 and returns the
// frame to resume: tvec.S stores it back into sscratch and restores the
// registers and the CSRs from it. Once this path calls the scheduler
// (switch_thread, shed/switch.h), that returned frame may belong to another
// thread, which is how a trap becomes a thread switch.
extern "C" trap_frame* usertrap(trap_frame* frame) {
    uint64_t cause = csrr(scause);

    SEELE_ASSERT((csrr(sstatus) & csr::sstatus::SPP) == 0, "Not from user mode");

    LOG_DEBUG("hart {} usertrap time = {:#x} pc = {:#x} status = {:#x} cause = {:#x}",
              cpuid(),
              csrr(time),
              frame->u_epc,
              frame->u_status,
              cause);

    // A user-mode external interrupt is one of the current thread's sources,
    // exactly like the kernel-side one below; the trap frame does not change
    // what the controller has to do.
    if(cause == csr::scause::EXT_INT) {
        service_external();
    }
    return frame;  // trapframe to resume
}

// Traps taken while already in the kernel: tvec.S's sscratch == 0 path, which
// saved the caller-saved registers on the current thread's kernel stack. The
// trap path is the scheduling point regardless of which mode the trap came
// from.
extern "C" void kerneltrap() {
    uint64_t pc = csrr(sepc);
    uint64_t cause = csrr(scause);

    // sstatus is hart state, not thread state. A trap is a scheduling point, so
    // this handler can hand the hart to another thread and not continue until
    // that thread has gone through traps of its own -- every one of which
    // rewrites SPP, SPIE and SIE. Keeping the interrupted value here and
    // restoring it before the sret below is what makes the return resume the
    // mode and interrupt state this trap interrupted, no matter how many
    // switches happened in between (the same reason sepc is kept in `pc`).
    const uint64_t status = csrr(sstatus);

    SEELE_ASSERT(status & csr::sstatus::SPP, "Not from supervisor mode");

    // Timer ticks are the scheduler's common case and would flood the console
    // at every rotation; external interrupts are the UART's receive and
    // transmit-empty conditions now, which are routine too. Log the causes that
    // are actually unusual: exceptions and the unhandled interrupt numbers.
    // Logging an external interrupt here would also feed the UART's own
    // transmit interrupt, one log line per serviced byte.
    if(cause != csr::scause::TIME_INT && cause != csr::scause::EXT_INT) {
        LOG_DEBUG("hart {} kerneltrap time = {:#x} pc = {:#x} status = {:#x} cause = {:#x}",
                  cpuid(),
                  csrr(time),
                  pc,
                  status,
                  cause);
    }

    switch(cause) {
        case csr::scause::TIME_INT: {
            // Re-arm the timer before scheduling: whichever thread resumes
            // must find a future deadline, not an expired one waiting to fire
            // the moment it returns.
            csrw(stimecmp, csrr(time) + 1000000);

            shed::schedule();
            break;
        }
        case csr::scause::EXT_INT: {
            service_external();
            // An interrupt may have woken a thread -- the console's receive
            // interrupt is what resumes a reader parked in console::read(). The
            // woken thread belongs to this hart's ready list, so switching here
            // returns to it now instead of leaving it until the next timer tick,
            // which is the difference between an echo within microseconds and one
            // within a timer period. schedule() returns immediately when nothing
            // is ready.
            shed::schedule();
            break;
        }
        default: {
            // Exceptions and any other interrupt cause: they have no handler
            // yet, so they are only logged. The assert above already caught
            // the "trap from user mode in the kernel path" case.
            break;
        }
    }

    // Restore sepc and sstatus, then let tvec.S's sret return. With sstatus back
    // to the value trap entry left, SPIE holds the interrupted thread's SIE and
    // SPP names the mode it came from, so sret restores interrupts and returns to
    // supervisor mode. Writing the saved value also undoes whatever another
    // thread's trap left in these fields while this one was switched out.
    csrw(sepc, pc);
    csrw(sstatus, status);
}

void set_external_controller(ExternalController& controller) noexcept {
    g_controller = &controller;
}

bool register_external_handler(uint32_t irq, ExternalHandler& handler, uint32_t priority) noexcept {
    if(irq == 0) {
        return false;
    }
    if(g_handler_count == MAX_EXTERNAL_HANDLERS) {
        return false;
    }
    // A source has exactly one owner; a second registration is a bug in one of
    // the two drivers, not something to arbitrate silently.
    for(uint32_t i = 0; i < g_handler_count; ++i) {
        if(g_handlers[i].irq == irq) {
            return false;
        }
    }
    // Without a controller the source would never be delivered, so refuse the
    // registration instead of leaving a handler that cannot run.
    if(g_controller == nullptr) {
        return false;
    }

    // Publish the handler first: an interrupt that arrives while enable() runs
    // then at least finds an entry (interrupts are off during probe anyway).
    // A refused source rolls the entry back so the table stays consistent.
    g_handlers[g_handler_count++] = registration{irq, &handler};
    if(!g_controller->enable(irq, priority)) {
        --g_handler_count;
        return false;
    }
    return true;
}

void dispatch_external_handler(uint32_t irq) noexcept {
    for(uint32_t i = 0; i < g_handler_count; ++i) {
        if(g_handlers[i].irq == irq) {
            g_handlers[i].handler->handle(irq);
            return;
        }
    }

    // No owner. The controller completes the claim regardless, so a source left
    // enabled by firmware keeps interrupting; the one-shot warning makes that
    // discoverable without turning it into a console storm.
    warn_unhandled_source(irq);
}

}  // namespace trap
