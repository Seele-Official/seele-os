#pragma once

// Single-use downward counter synchronization primitive (mirroring C++20
// `std::latch`) built on Spinlock and WaitQueue.
//
// Blocking threads keep their WaitQueue::waiter node on their own kernel stack,
// so Latch allocates no heap memory and is trivially destructible (safe in
// static storage).
//
// Once the internal counter reaches zero, it stays zero permanently: all
// blocked waiters are broadcast-woken and any subsequent wait() / try_wait()
// returns immediately without blocking. `count_down()` uses only `m_lock`
// (never sleeps), so it is safe from any hart, interrupt handler, or
// interrupt-masked post-switch reclamation context.
//
// Invariant under `m_lock`: `(m_count > 0) || m_waiters.empty()`.

#include <cstdint>
#include <type_traits>

#include "concurrency/spinlock.h"
#include "concurrency/wait_queue.h"

class Latch {
public:
    // Construct a single-use latch with initial count 1 (one-shot completion) or
    // an explicit `expected` count.
    constexpr Latch() noexcept : m_count(1) {}

    explicit constexpr Latch(uint64_t expected) noexcept : m_count(expected) {}

    ~Latch() noexcept = default;

    Latch(const Latch&) = delete;
    Latch& operator= (const Latch&) = delete;
    Latch(Latch&&) = delete;
    Latch& operator= (Latch&&) = delete;

    // Decrement the counter by `n` without blocking; when the counter reaches
    // zero, wake all blocked waiters. Safe from any hart, interrupt context, or
    // with local interrupts disabled. Panics if `n > m_count`.
    void count_down(uint64_t n = 1) noexcept;

    // Non-blocking check: returns true iff the counter has reached zero. Safe
    // from thread or interrupt context.
    [[nodiscard]] bool try_wait() noexcept;

    // Block the current non-idle thread until the counter reaches zero; returns
    // immediately if already zero. Requires local interrupts enabled and no
    // spinlock held.
    void wait() noexcept;

    // Decrement the counter by `n` and block until it reaches zero (waking all
    // waiters if this call drops the counter to zero). Requires a non-idle
    // thread with local interrupts enabled and no spinlock held. Panics if
    // `n > m_count`.
    void arrive_and_wait(uint64_t n = 1) noexcept;

private:
    Spinlock m_lock{};
    uint64_t m_count;
    WaitQueue m_waiters{};
};

static_assert(std::is_trivially_destructible_v<Latch>,
              "Latch must be trivially destructible for static storage");
