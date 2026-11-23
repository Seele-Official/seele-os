#include "concurrency/spinlock.h"

#include <atomic>
#include <cstdint>

#include "hart.h"
#include "panic.h"
#include "riscv.h"

namespace {

// Disable interrupts, remembering whether they were on. Only the outermost
// push_off() captures the state, so nested critical sections keep them off.
// intr_save() reads and clears SIE in one CSR instruction, so no trap can run
// between the capture and the disable.
void push_off() noexcept {
    auto cpu = mycpu();
    const bool interrupts_were_on = riscv::intr_save();
    if(cpu->interrupt_depth == 0) {
        cpu->interrupts_were_enabled = interrupts_were_on;
    }

    ++cpu->interrupt_depth;
}

// Undo one push_off(), restoring the interrupt state the outermost lock found
// only when this hart holds no other lock. An underflow means unlock() ran
// without a matching lock(); the hart's interrupt bookkeeping is already
// inconsistent, so report it instead of wrapping around.
void pop_off() noexcept {
    auto cpu = mycpu();
    SEELE_ASSERT(cpu->interrupt_depth > 0);
    --cpu->interrupt_depth;
    if(cpu->interrupt_depth == 0) {
        riscv::intr_restore(cpu->interrupts_were_enabled);
    }
}

}  // namespace

void Spinlock::lock() noexcept {
    SEELE_ASSERT(m_owner.load(std::memory_order_relaxed) != cpuid(),
                 "spinlock {:#018x}: recursive lock",
                 reinterpret_cast<uintptr_t>(this));

    push_off();

    while(true) {
        bool expected = false;
        if(m_locked.compare_exchange_weak(expected,
                                          true,
                                          std::memory_order_acquire,
                                          std::memory_order_relaxed)) {
            m_owner.store(cpuid(), std::memory_order_relaxed);
            return;
        }
        // Another hart owns the lock. Interrupts are off on this hart, so there
        // is nothing to do but retry; the Zihintpause `pause` hint cannot be
        // assumed on every hart the kernel supports.
    }
}

bool Spinlock::owned_by_current_hart() const noexcept {
    return m_owner.load(std::memory_order_relaxed) == cpuid();
}

bool Spinlock::try_lock() noexcept {
    // Self-ownership is reported as "busy", not as the recursive-lock
    // assertion: the caller (the panic path) is allowed to proceed without the
    // lock. The owner read is relaxed for the same reason as in lock(): it only
    // tells "mine" from "someone else's" while diagnosing a hang that has
    // already happened, and the CAS below is what actually acquires the lock.
    if(m_owner.load(std::memory_order_relaxed) == cpuid()) {
        return false;
    }

    push_off();

    bool expected = false;
    if(!m_locked.compare_exchange_strong(expected,
                                         true,
                                         std::memory_order_acquire,
                                         std::memory_order_relaxed)) {
        // Another hart owns the lock. Undo the local interrupt bookkeeping
        // before reporting the failure, so a caller that gives up does not
        // leave interrupts disabled.
        pop_off();
        return false;
    }

    m_owner.store(cpuid(), std::memory_order_relaxed);
    return true;
}

void Spinlock::unlock() noexcept {
    SEELE_ASSERT(m_owner.load(std::memory_order_relaxed) == cpuid(),
                 "spinlock {:#018x}: unlock by a hart that does not own it",
                 reinterpret_cast<uintptr_t>(this));
    m_owner.store(NO_OWNER, std::memory_order_relaxed);
    m_locked.store(false, std::memory_order_release);
    pop_off();
}
