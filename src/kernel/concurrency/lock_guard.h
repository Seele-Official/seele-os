#pragma once

// RAII owner for a lock.
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
