#pragma once

// Counting semaphore with FIFO permit handoff built on Spinlock and WaitQueue.
//
// Blocking threads keep their WaitQueue::waiter node on their own kernel stack,
// so Semaphore allocates no heap memory and is trivially destructible (safe in
// static storage).
//
// Invariant under `m_lock`: `(m_count == 0) || m_waiters.empty()`. When
// release() finds a waiting thread, it hands the permit directly to the oldest
// waiter without incrementing `m_count`, avoiding barging and thundering herds.

#include <cstdint>
#include <type_traits>

#include "concurrency/spinlock.h"
#include "concurrency/wait_queue.h"

class Semaphore {
public:
    explicit constexpr Semaphore(uint64_t initial = 0) noexcept : m_count(initial) {}

    ~Semaphore() noexcept = default;

    Semaphore(const Semaphore&) = delete;
    Semaphore& operator= (const Semaphore&) = delete;
    Semaphore(Semaphore&&) = delete;
    Semaphore& operator= (Semaphore&&) = delete;

    // Acquire one permit, sleeping until one is available.
    // Must be called from a non-idle thread with interrupts enabled and no
    // spinlock held.
    void acquire() noexcept;

    // Acquire one permit without blocking. Returns true on success, false if no
    // permit is currently available. Safe from thread or interrupt context.
    [[nodiscard]] bool try_acquire() noexcept;

    // Release one permit, waking the oldest waiter via direct handoff or
    // incrementing the permit count if no thread is waiting. Safe from any hart
    // and from interrupt context. Panics on uint64_t count overflow.
    void release() noexcept;

private:
    Spinlock m_lock{};
    uint64_t m_count;
    WaitQueue m_waiters{};
};

static_assert(std::is_trivially_destructible_v<Semaphore>,
              "Semaphore must be trivially destructible for static storage");
