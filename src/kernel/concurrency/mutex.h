#pragma once

// Sleepable non-recursive mutex with FIFO ownership handoff built on Spinlock
// and WaitQueue.
//
// Blocking threads keep their WaitQueue::waiter node on their own kernel stack,
// so Mutex allocates no heap memory and is trivially destructible (safe in
// static storage).
//
// Unlike Semaphore, Mutex enforces strict thread ownership: only the thread
// that acquired it may unlock it, and recursive lock() panics rather than
// self-deadlocking. Works directly with LockGuard<Mutex>.
//
// Invariant under `m_lock`: `(m_owner != nullptr) || m_waiters.empty()`. When
// unlock() finds a waiting thread, it transfers `m_owner` directly to the
// oldest waiter before releasing `m_lock` and waking it, preventing barging
// and thundering herds.

#include <type_traits>

#include "concurrency/spinlock.h"
#include "concurrency/wait_queue.h"
#include "proc/process.h"

class Mutex {
public:
    constexpr Mutex() noexcept = default;
    ~Mutex() noexcept = default;

    Mutex(const Mutex&) = delete;
    Mutex& operator= (const Mutex&) = delete;
    Mutex(Mutex&&) = delete;
    Mutex& operator= (Mutex&&) = delete;

    // Acquire ownership, sleeping until the mutex is available.
    // Must be called from a non-idle thread with interrupts enabled and no
    // spinlock held. Panics on recursive lock by the current owner.
    void lock() noexcept;

    // Try to acquire ownership without blocking. Returns true on success, or
    // false if the mutex is already owned (including by the current thread).
    // Must be called from a non-idle thread context (not from interrupt
    // handlers).
    [[nodiscard]] bool try_lock() noexcept;

    // Release ownership, handing it directly to the oldest waiter if any, or
    // leaving the mutex unlocked. Must be called by the owning thread (never
    // from an interrupt handler); panics if the current thread does not own
    // the mutex. Safe when local interrupts are masked (e.g. during two-phase
    // wait handoffs).
    void unlock() noexcept;

private:
    Spinlock m_lock{};
    proc::thread* m_owner = nullptr;
    WaitQueue m_waiters{};
};

static_assert(std::is_trivially_destructible_v<Mutex>,
              "Mutex must be trivially destructible for static storage");
