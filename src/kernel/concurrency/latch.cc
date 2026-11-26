#include "concurrency/latch.h"

#include <cstdint>

#include "hart.h"
#include "panic.h"
#include "riscv.h"
#include "concurrency/lock_guard.h"
#include "sched/schedule.h"

void Latch::count_down(uint64_t n) noexcept {
    if(n == 0) {
        return;
    }

    WaitQueue woken{};
    {
        LockGuard guard{m_lock};
        SEELE_ASSERT(n <= m_count, "latch: count_down({}) exceeds remaining count {}", n, m_count);
        m_count -= n;
        if(m_count == 0) {
            m_waiters.take_all(woken);
        }
    }

    // Wake after releasing m_lock so m_lock never nests with cpu->lock.
    woken.wake_all();
}

bool Latch::try_wait() noexcept {
    LockGuard guard{m_lock};
    return m_count == 0;
}

void Latch::wait() noexcept {
    const cpu* hart = mycpu();
    SEELE_ASSERT(hart->current != nullptr, "latch: wait() without a current thread");
    SEELE_ASSERT(hart->current != hart->idle, "latch: wait() from the idle thread");
    SEELE_ASSERT(hart->interrupt_depth == 0, "latch: wait() while holding a spinlock");
    SEELE_ASSERT(riscv::is_intr_on(), "latch: wait() with interrupts disabled");

    WaitQueue::waiter entry{};
    bool ready = false;

    // Mask local interrupts before taking m_lock so releasing m_lock keeps
    // SIE = 0 through sched::sleep().
    riscv::intr_off();
    {
        LockGuard guard{m_lock};
        if(m_count == 0) {
            SEELE_DASSERT(m_waiters.empty(), "latch: waiters present when count is zero");
            ready = true;
        } else {
            m_waiters.prepare_wait(entry);
        }
    }

    if(!ready) {
        // count_down() unlinks `entry` under m_lock only when m_count reaches
        // zero, and the count never increases again, so a single sleep() is
        // sufficient.
        sched::sleep();
    }

    riscv::intr_on();
}

void Latch::arrive_and_wait(uint64_t n) noexcept {
    const cpu* hart = mycpu();
    SEELE_ASSERT(hart->current != nullptr, "latch: arrive_and_wait() without a current thread");
    SEELE_ASSERT(hart->current != hart->idle, "latch: arrive_and_wait() from the idle thread");
    SEELE_ASSERT(hart->interrupt_depth == 0, "latch: arrive_and_wait() while holding a spinlock");
    SEELE_ASSERT(riscv::is_intr_on(), "latch: arrive_and_wait() with interrupts disabled");

    WaitQueue woken{};
    WaitQueue::waiter entry{};
    bool should_sleep = false;

    riscv::intr_off();
    {
        LockGuard guard{m_lock};
        SEELE_ASSERT(n <= m_count,
                     "latch: arrive_and_wait({}) exceeds remaining count {}",
                     n,
                     m_count);
        m_count -= n;
        if(m_count == 0) {
            m_waiters.take_all(woken);
        } else {
            m_waiters.prepare_wait(entry);
            should_sleep = true;
        }
    }

    if(should_sleep) {
        sched::sleep();
        riscv::intr_on();
        return;
    }

    riscv::intr_on();
    woken.wake_all();
}
