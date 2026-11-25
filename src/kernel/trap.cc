#include "trap.h"

#include <cstdint>

#include "console.h"
#include "hart.h"
#include "panic.h"
#include "riscv.h"
#include "sched/schedule.h"

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
// to the next thread (sched::switch_thread, sched/switch.h), so this is where the
// kernel preempts a thread or resumes one an interrupt woke. (A thread can also
// give the hart away on purpose, in sched::sleep().) Both interrupt causes the
// kernel handles are scheduling points: the timer preempts the running thread
// (sched::schedule), and an external interrupt may have woken one -- the console's
// receive interrupt wakes a reader parked in console::read() -- so it schedules
// too. Exceptions never schedule.

// Traps that arrived from user mode: tvec.S's sscratch != 0 path, which
// switched to this thread's kernel stack, built a 272-byte trap_frame there,
// cleared sscratch, and restored kernel tp. The handler receives that frame
// in a0 and returns the current hart's trap_scratch* (which may differ from
// entry if scheduling migrated the thread); tvec.S falls through into tvecret
// to update kernel_sp, arm sscratch, restore the frame, and sret.
extern "C" trap_scratch* usertrap(trap_frame* frame) {
    SEELE_ASSERT((frame->sstatus & csr::sstatus::SPP) == 0, "Not from user mode");

    const uint64_t cause = csrr(scause);

    LOG_DEBUG("hart {} usertrap time = {:#x} pc = {:#x} status = {:#x} cause = {:#x}",
              cpuid(),
              csrr(time),
              frame->sepc,
              frame->sstatus,
              cause);

    // A user-mode external interrupt is one of the current thread's sources,
    // exactly like the kernel-side one below; the trap frame does not change
    // what the controller has to do.
    if(cause == csr::scause::EXT_INT) {
        service_external();
    }

    SEELE_ASSERT(!is_intr_on(), "usertrap returning with interrupts enabled");
    return &mycpu()->trap_scratch;
}

// Traps taken while already in the kernel: tvec.S's sscratch == 0 path, which
// saved the full trap_frame (x1..x31, sepc, sstatus) on the current thread's
// kernel stack and restores sepc/sstatus from it on return, keeping each
// thread's interrupted state intact across preemptive switches.
extern "C" void kerneltrap(trap_frame* frame) {
    SEELE_ASSERT((frame->sstatus & csr::sstatus::SPP) != 0, "Not from supervisor mode");

    const uint64_t cause = csrr(scause);

    switch(cause) {
        case csr::scause::TIME_INT: {
            // Re-arm the timer before scheduling: whichever thread resumes
            // must find a future deadline, not an expired one waiting to fire
            // the moment it returns.
            csrw(stimecmp, csrr(time) + 1000000);

            sched::schedule();
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
            sched::schedule();
            break;
        }
        default: {
            // Returning to the same sepc without advancing it or clearing the
            // interrupt source would trap again immediately in an infinite loop.
            const uint64_t tval = csrr(stval);
            const char* kind = (cause & csr::scause::INTERRUPT) != 0 ? "interrupt" : "exception";
            SEELE_PANIC(
                "hart {} unhandled kernel {}: cause={:#x} sepc={:#018x} stval={:#018x} sstatus={:#x}",
                cpuid(),
                kind,
                cause,
                frame->sepc,
                tval,
                frame->sstatus);
        }
    }
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
