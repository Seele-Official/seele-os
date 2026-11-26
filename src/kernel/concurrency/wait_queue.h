#pragma once

// Externally synchronized FIFO wait queue for blocking synchronization
// primitives (Semaphore, Mutex, Condvar, drivers).
//
// Waiters are stack-allocated intrusive nodes owned by the blocking thread's
// kernel stack frame across sched::sleep(): zero heap allocation, no OOM path,
// and WaitQueue is trivially destructible (safe in static storage).
//
// WaitQueue carries no internal lock. The enclosing primitive's Spinlock must
// protect both its condition state and its WaitQueue so condition checking and
// enqueueing are atomic. Wakers must unlink waiters (pop_one / take_all) while
// holding that lock and call sched::wake() only after releasing it, keeping
// primitive locks out of the scheduler's cpu->lock hierarchy and ensuring a
// resumed thread never unwinds a waiter that is still linked.

#include <cstddef>
#include <type_traits>

#include "proc/process.h"
#include "util/list.h"

class WaitQueue {
public:
    // Stack-allocated intrusive link representing one blocked thread.
    struct waiter {
        waiter* next = nullptr;
        waiter* prev = nullptr;
        proc::thread* thread = nullptr;
    };

    static_assert(intrusive_node<waiter>,
                  "WaitQueue::waiter must satisfy IntrusiveList's link contract");

    constexpr WaitQueue() noexcept = default;
    ~WaitQueue() noexcept = default;

    WaitQueue(const WaitQueue&) = delete;
    WaitQueue& operator= (const WaitQueue&) = delete;
    WaitQueue(WaitQueue&&) = delete;
    WaitQueue& operator= (WaitQueue&&) = delete;

    [[nodiscard]] bool empty() const noexcept {
        return m_waiters.empty();
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return m_waiters.size();
    }

    // Transition the current thread to ThreadState::Blocking via
    // sched::prepare_sleep() and append `entry` to the tail of this queue.
    //
    // Preconditions:
    // - Caller holds the external Spinlock protecting this WaitQueue and has
    //   already masked local interrupts outside that lock so SIE stays 0 after
    //   unlocking through sched::sleep().
    // - Current thread is a non-idle thread in ThreadState::Running.
    // - `entry` is unlinked and remains alive on the current thread's stack
    //   until after sched::sleep() returns.
    void prepare_wait(waiter& entry) noexcept;

    // Unlink the oldest waiter and return its thread, or nullptr if empty.
    // Caller holds the external Spinlock protecting this WaitQueue and should
    // wake the returned thread with sched::wake() after releasing that lock.
    [[nodiscard]] proc::thread* pop_one() noexcept;

    // Move all waiters in FIFO order into `destination` (which must be empty)
    // while holding the external Spinlock, so the caller can release the lock
    // and then wake every waiter with `destination.wake_all()`.
    void take_all(WaitQueue& destination) noexcept;

    // Unlink every waiter in this queue and wake its thread via sched::wake().
    // Call only on a thread-local drained queue after releasing the external
    // lock (never concurrently with other operations on the same WaitQueue).
    void wake_all() noexcept;

private:
    IntrusiveList<waiter> m_waiters{};
};

static_assert(std::is_trivially_destructible_v<WaitQueue>,
              "WaitQueue must be trivially destructible for static storage");
