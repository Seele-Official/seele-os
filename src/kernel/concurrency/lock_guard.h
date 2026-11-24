#pragma once

#include <atomic>

#include "panic.h"

// RAII owners for locks.
//
// LockGuard acquires in its constructor and releases in its destructor, so
// every early return and every error path leaves the critical section. It is
// deliberately non-copyable and non-movable: ownership of a scoped lock cannot
// outlive the scope that acquired it.
//
// The interface is generic: it only requires lock()/unlock(). The lock's lock()
// must be noexcept; Spinlock's lock() also disables interrupts and nests
// safely.
template <typename Lock>
class LockGuard {
public:
    explicit LockGuard(Lock& lock) noexcept : m_lock(lock) {
        m_lock.lock();
    }

    ~LockGuard() noexcept {
        m_lock.unlock();
    }

    LockGuard(const LockGuard&) = delete;
    LockGuard& operator= (const LockGuard&) = delete;
    LockGuard(LockGuard&&) = delete;
    LockGuard& operator= (LockGuard&&) = delete;

private:
    Lock& m_lock;
};

// TryLockGuard is the non-blocking variant: it asks the lock once and owns it
// only when that call succeeds. It exists for paths that must not wait for a
// lock a panicking hart may be parked inside -- the panic path and the writers
// that detect the panic latch while waiting. The caller checks locked() and
// either enters the critical section or takes its non-locking fallback.
//
// The interface needs try_lock()/unlock(); Spinlock::try_lock() also disables
// interrupts on success, and a failed attempt leaves the lock's own state
// untouched.
template <typename Lock>
class TryLockGuard {
public:
    explicit TryLockGuard(Lock& lock) noexcept : m_lock(lock), m_locked(lock.try_lock()) {}

    ~TryLockGuard() noexcept {
        if(m_locked) {
            m_lock.unlock();
        }
    }

    TryLockGuard(const TryLockGuard&) = delete;
    TryLockGuard& operator= (const TryLockGuard&) = delete;
    TryLockGuard(TryLockGuard&&) = delete;
    TryLockGuard& operator= (TryLockGuard&&) = delete;

    [[nodiscard]] bool locked() const noexcept {
        return m_locked;
    }

private:
    Lock& m_lock;
    bool m_locked;
};

// Acquires `lock` unless `panicking` is latched.
//
// Spins via try_lock(), asserting against self-deadlock on the current hart. If
// `panicking` is observed before acquisition or re-checked as true immediately
// after acquiring the lock, releases the lock (if taken) and leaves locked()
// false so the caller takes its lock-free panic fallback.
template <typename Lock>
class PanicLockGuard {
public:
    PanicLockGuard(Lock& lock,
                   const std::atomic<bool>& panicking,
                   const char* recursive_message) noexcept : m_lock(lock) {
        while(true) {
            if(panicking.load(std::memory_order_acquire)) {
                return;
            }
            if(m_lock.try_lock()) {
                if(panicking.load(std::memory_order_acquire)) {
                    m_lock.unlock();
                    return;
                }
                m_locked = true;
                return;
            }
            SEELE_ASSERT(!m_lock.owned_by_current_hart(), "{}", recursive_message);
        }
    }

    ~PanicLockGuard() noexcept {
        if(m_locked) {
            m_lock.unlock();
        }
    }

    PanicLockGuard(const PanicLockGuard&) = delete;
    PanicLockGuard& operator= (const PanicLockGuard&) = delete;
    PanicLockGuard(PanicLockGuard&&) = delete;
    PanicLockGuard& operator= (PanicLockGuard&&) = delete;

    [[nodiscard]] bool locked() const noexcept {
        return m_locked;
    }

private:
    Lock& m_lock;
    bool m_locked = false;
};
