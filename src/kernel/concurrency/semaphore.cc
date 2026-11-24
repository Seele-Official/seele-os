#include "concurrency/semaphore.h"

#include <limits>

#include "hart.h"
#include "panic.h"
#include "riscv.h"
#include "concurrency/lock_guard.h"
#include "sched/schedule.h"

void Semaphore::acquire() noexcept {
    const cpu* hart = mycpu();
    SEELE_ASSERT(hart->current != nullptr, "semaphore: acquire() without a current thread");
    SEELE_ASSERT(hart->current != hart->idle, "semaphore: acquire() from the idle thread");
    SEELE_ASSERT(hart->interrupt_depth == 0, "semaphore: acquire() while holding a spinlock");
    SEELE_ASSERT(riscv::is_intr_on(), "semaphore: acquire() with interrupts disabled");

    WaitQueue::waiter entry{};
    bool acquired = false;

    // Disable local interrupts before taking m_lock so unlocking m_lock leaves
    // SIE = 0 through sched::sleep(). That keeps local preemption from running
    // while this thread is in ThreadState::Blocking; concurrent cross-hart
    // releases synchronize through the Blocking -> Running handshake.
    riscv::intr_off();
    {
        LockGuard guard{m_lock};
        if(m_count > 0) {
            SEELE_DASSERT(m_waiters.empty(), "semaphore: waiters present with positive count");
            --m_count;
            acquired = true;
        } else {
            m_waiters.prepare_wait(entry);
        }
    }

    if(!acquired) {
        // release() unlinked `entry` under m_lock before calling sched::wake()
        // and transferred one permit directly to this thread.
        sched::sleep();
    }

    riscv::intr_on();
}

bool Semaphore::try_acquire() noexcept {
    LockGuard guard{m_lock};
    if(m_count == 0) {
        return false;
    }
    SEELE_DASSERT(m_waiters.empty(), "semaphore: waiters present with positive count");
    --m_count;
    return true;
}

void Semaphore::release() noexcept {
    proc::thread* woken = nullptr;
    {
        LockGuard guard{m_lock};
        woken = m_waiters.pop_one();
        if(woken == nullptr) {
            SEELE_ASSERT(m_count < std::numeric_limits<uint64_t>::max(),
                         "semaphore: permit count overflow");
            ++m_count;
        } else {
            SEELE_ASSERT(m_count == 0, "semaphore: waiters present with positive count");
        }
    }

    // Wake after releasing m_lock so m_lock never nests with cpu->lock.
    if(woken != nullptr) {
        sched::wake(woken);
    }
}
