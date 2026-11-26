#include "concurrency/mutex.h"

#include <cstdint>

#include "hart.h"
#include "panic.h"
#include "riscv.h"
#include "concurrency/lock_guard.h"
#include "sched/schedule.h"

void Mutex::lock() noexcept {
    const cpu* hart = mycpu();
    SEELE_ASSERT(hart->current != nullptr, "mutex: lock() without a current thread");
    SEELE_ASSERT(hart->current != hart->idle, "mutex: lock() from the idle thread");
    SEELE_ASSERT(hart->interrupt_depth == 0, "mutex: lock() while holding a spinlock");
    SEELE_ASSERT(riscv::is_intr_on(), "mutex: lock() with interrupts disabled");

    proc::thread* const self = hart->current;
    WaitQueue::waiter entry{};
    bool acquired = false;

    // Disable local interrupts before taking m_lock so unlocking m_lock leaves
    // SIE = 0 through sched::sleep(). That keeps local preemption from running
    // while this thread is in ThreadState::Blocking; concurrent cross-hart
    // unlocks synchronize through the Blocking -> Running handshake.
    riscv::intr_off();
    {
        LockGuard guard{m_lock};
        SEELE_ASSERT(m_owner != self,
                     "mutex {:#018x}: recursive lock",
                     reinterpret_cast<uintptr_t>(this));
        if(m_owner == nullptr) {
            SEELE_DASSERT(m_waiters.empty(), "mutex: waiters present while unlocked");
            m_owner = self;
            acquired = true;
        } else {
            m_waiters.prepare_wait(entry);
        }
    }

    if(!acquired) {
        // unlock() unlinked `entry` under m_lock before calling sched::wake()
        // and transferred ownership directly to `self`.
        sched::sleep();
    }

    riscv::intr_on();
}

bool Mutex::try_lock() noexcept {
    LockGuard guard{m_lock};
    const cpu* hart = mycpu();
    SEELE_ASSERT(hart->current != nullptr, "mutex: try_lock() without a current thread");
    SEELE_ASSERT(hart->current != hart->idle, "mutex: try_lock() from the idle thread");
    if(m_owner != nullptr) {
        return false;
    }
    SEELE_DASSERT(m_waiters.empty(), "mutex: waiters present while unlocked");
    m_owner = hart->current;
    return true;
}

void Mutex::unlock() noexcept {
    proc::thread* woken = nullptr;
    {
        LockGuard guard{m_lock};
        const cpu* hart = mycpu();
        SEELE_ASSERT(hart->current != nullptr, "mutex: unlock() without a current thread");
        SEELE_ASSERT(hart->current != hart->idle, "mutex: unlock() from the idle thread");
        SEELE_ASSERT(m_owner == hart->current,
                     "mutex {:#018x}: unlock by a thread that does not own it",
                     reinterpret_cast<uintptr_t>(this));

        woken = m_waiters.pop_one();
        // Transfer ownership directly to the oldest waiter (or clear m_owner
        // when the queue is empty) so no newcomer can barge ahead of it.
        m_owner = woken;
    }

    // Wake after releasing m_lock so m_lock never nests with cpu->lock.
    if(woken != nullptr) {
        sched::wake(woken);
    }
}
