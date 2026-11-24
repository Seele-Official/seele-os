#pragma once

#include <atomic>
#include <cstdint>

// A kernel spinlock.
//
// Follows the shared-memory model: lock() spins until it owns the lock, and
// ownership is published with acquire/release ordering so that the critical
// section cannot move across it.
//
// lock() also disables S-mode interrupts on the current hart. Without that, an
// interrupt handler that takes the same lock would spin forever on a hart whose
// interrupted code owns the lock. Locks nest: only the outermost unlock()
// restores the interrupt state the outermost lock() found, so an inner critical
// section never turns interrupts back on early. The nesting bookkeeping lives in
// concurrency/spinlock.cc and is per-hart state.
//
// Misuse panics instead of corrupting state: locking a lock this hart already
// owns, unlocking a lock this hart does not own, and unwinding the interrupt
// depth past zero all stop the kernel with a stack trace. The checks are cheap
// (one relaxed load per operation) and only compare hart ids, so a lock held by
// another hart is still a normal spin, never a false positive.
//
// Only usable once the kernel runs in S-mode: the interrupt flag is sstatus.SIE.
class Spinlock {
public:
    Spinlock() = default;
    Spinlock(const Spinlock&) = delete;
    Spinlock& operator= (const Spinlock&) = delete;

    // Acquire the lock with interrupts disabled, spinning while another hart
    // owns it. Panics if this hart already owns it: that is a self-deadlock,
    // not something to wait for.
    void lock() noexcept;

    // Release the lock and, when this was the outermost lock on this hart,
    // restore the interrupt-enable state that lock() found. Panics if this hart
    // does not own the lock.
    void unlock() noexcept;

    // Whether the current hart already owns this lock. try_lock() reports such a
    // lock as "busy" rather than panicking (the panic path may not recurse into
    // the misuse check), so a caller that retries with try_lock has no way to
    // tell "another hart has it" from "this hart has it" without asking. A
    // retry loop that sees true can never make progress and should report it.
    [[nodiscard]] bool owned_by_current_hart() const noexcept;

    // Acquire the lock only if it is free: true when this call owns it (with
    // the same interrupt-disable bookkeeping as lock()), false when another
    // hart owns it. A lock this hart already owns also reports false instead of
    // panicking, because try_lock() exists for best-effort paths -- the panic
    // path is allowed to continue without the lock rather than recurse into the
    // misuse check. Never spins.
    [[nodiscard]] bool try_lock() noexcept;

private:
    // Sentinel for "no hart owns the lock". cpuid values are bounded by
    // MAX_HARTS, so the largest uint64_t cannot collide with a real one.
    static constexpr uint64_t NO_OWNER = ~uint64_t{0};

    // Single atomic holding both lock state and owner cpuid: NO_OWNER when
    // unlocked, otherwise the owning hart's cpuid. Combining them eliminates
    // the split-state window between acquiring the lock and recording its owner.
    std::atomic<uint64_t> m_owner{NO_OWNER};
};
