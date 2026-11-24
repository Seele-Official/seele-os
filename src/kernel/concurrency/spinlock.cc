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
    const bool interrupts_were_on = riscv::intr_save();
    auto* cpu = mycpu();
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
    auto* cpu = mycpu();
    SEELE_ASSERT(cpu->interrupt_depth > 0);
    --cpu->interrupt_depth;
    if(cpu->interrupt_depth == 0) {
        riscv::intr_restore(cpu->interrupts_were_enabled);
    }
}

}  // namespace

void Spinlock::lock() noexcept {
    const uint64_t self = cpuid();
    SEELE_ASSERT(m_owner.load(std::memory_order_relaxed) != self,
                 "spinlock {:#018x}: recursive lock",
                 reinterpret_cast<uintptr_t>(this));

    push_off();

    while(true) {
        // Test-and-test-and-set: spin with relaxed loads while another hart
        // owns the lock so contended harts keep the cache line shared instead
        // of bouncing it in modified state on every iteration.
        while(m_owner.load(std::memory_order_relaxed) != NO_OWNER) {}

        uint64_t expected = NO_OWNER;
        if(m_owner.compare_exchange_weak(expected,
                                         self,
                                         std::memory_order_acquire,
                                         std::memory_order_relaxed)) {
            return;
        }
    }
}

void Spinlock::unlock() noexcept {
    SEELE_ASSERT(m_owner.load(std::memory_order_relaxed) == cpuid(),
                 "spinlock {:#018x}: unlock by a hart that does not own it",
                 reinterpret_cast<uintptr_t>(this));
    m_owner.store(NO_OWNER, std::memory_order_release);
    pop_off();
}
