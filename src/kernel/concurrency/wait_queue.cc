#include "concurrency/wait_queue.h"

#include "hart.h"
#include "panic.h"
#include "riscv.h"
#include "sched/schedule.h"

void WaitQueue::prepare_wait(waiter& entry) noexcept {
    const cpu* hart = mycpu();
    SEELE_ASSERT(!riscv::is_intr_on(), "WaitQueue::prepare_wait() with interrupts enabled");
    SEELE_ASSERT(hart->current != nullptr, "WaitQueue::prepare_wait() without a current thread");
    SEELE_ASSERT(hart->current != hart->idle, "WaitQueue::prepare_wait() from the idle thread");
    SEELE_ASSERT(entry.next == nullptr && entry.prev == nullptr,
                 "WaitQueue::prepare_wait(): waiter is already linked");

    // Mark Blocking before exposing `entry` on the queue so any concurrent
    // waker that acquires the enclosing lock observes Blocking (or Blocked),
    // never Running.
    sched::prepare_sleep();
    entry.thread = hart->current;
    m_waiters.push_back(&entry);
}

proc::thread* WaitQueue::pop_one() noexcept {
    waiter* entry = m_waiters.pop_front();
    if(entry == nullptr) {
        return nullptr;
    }

    proc::thread* thread = entry->thread;
    entry->thread = nullptr;
    SEELE_ASSERT(thread != nullptr, "WaitQueue::pop_one(): waiter has no thread");
    return thread;
}

void WaitQueue::take_all(WaitQueue& destination) noexcept {
    SEELE_ASSERT(&destination != this, "WaitQueue::take_all(): cannot drain into self");
    SEELE_ASSERT(destination.empty(), "WaitQueue::take_all(): destination queue is not empty");
    while(waiter* entry = m_waiters.pop_front()) {
        destination.m_waiters.push_back(entry);
    }
}

void WaitQueue::wake_all() noexcept {
    while(proc::thread* thread = pop_one()) {
        sched::wake(thread);
    }
}
