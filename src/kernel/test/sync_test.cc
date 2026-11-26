#include <atomic>
#include <cstddef>
#include <cstdint>

#include "hart.h"
#include "riscv.h"
#include "concurrency/latch.h"
#include "concurrency/lock_guard.h"
#include "concurrency/mutex.h"
#include "concurrency/semaphore.h"
#include "concurrency/spinlock.h"
#include "test/ktest.h"

namespace {

KTEST_CASE(sync, spinlock_irq_nesting) {
    Spinlock outer{};
    Spinlock inner{};

    KTEST_ASSERT(riscv::is_intr_on());
    KTEST_ASSERT_EQ(mycpu()->interrupt_depth, uint32_t{0});

    {
        LockGuard g1{outer};
        KTEST_EXPECT(!riscv::is_intr_on());
        KTEST_EXPECT_EQ(mycpu()->interrupt_depth, uint32_t{1});

        {
            LockGuard g2{inner};
            KTEST_EXPECT(!riscv::is_intr_on());
            KTEST_EXPECT_EQ(mycpu()->interrupt_depth, uint32_t{2});
        }

        KTEST_EXPECT(!riscv::is_intr_on());
        KTEST_EXPECT_EQ(mycpu()->interrupt_depth, uint32_t{1});
    }

    KTEST_EXPECT(riscv::is_intr_on());
    KTEST_EXPECT_EQ(mycpu()->interrupt_depth, uint32_t{0});

    // Acquiring a lock while interrupts are already masked keeps them masked
    // on unlock.
    const bool prev = riscv::intr_save();
    {
        LockGuard g{outer};
        KTEST_EXPECT(!riscv::is_intr_on());
    }
    const bool still_off = !riscv::is_intr_on();
    riscv::intr_restore(prev);
    KTEST_EXPECT(still_off);
}

KTEST_CASE(sync, semaphore_nonblocking) {
    Semaphore sem{2};
    KTEST_EXPECT(sem.try_acquire());
    KTEST_EXPECT(sem.try_acquire());
    KTEST_EXPECT(!sem.try_acquire());

    sem.release();
    sem.acquire();
    KTEST_EXPECT(!sem.try_acquire());
}

KTEST_CASE(sync, mutex_basic) {
    Mutex mtx{};
    KTEST_ASSERT(riscv::is_intr_on());
    KTEST_ASSERT_EQ(mycpu()->interrupt_depth, uint32_t{0});

    KTEST_EXPECT(mtx.try_lock());
    KTEST_EXPECT(riscv::is_intr_on());
    KTEST_EXPECT_EQ(mycpu()->interrupt_depth, uint32_t{0});
    KTEST_EXPECT(!mtx.try_lock());
    mtx.unlock();

    {
        LockGuard guard{mtx};
        KTEST_EXPECT(riscv::is_intr_on());
        KTEST_EXPECT_EQ(mycpu()->interrupt_depth, uint32_t{0});
        KTEST_EXPECT(!mtx.try_lock());
    }

    KTEST_EXPECT(mtx.try_lock());
    mtx.unlock();
}

Mutex g_smp_mutex{};
std::atomic<bool> g_smp_mutex_in_cs{false};
std::atomic<size_t> g_smp_mutex_waiters_ready{0};
uint64_t g_smp_mutex_counter = 0;

KTEST_SMP_CASE(sync, smp_mutex_contention) {
    constexpr uint64_t ROUNDS_PER_HART = 3;
    const size_t harts = ktest::hart_count();

    if(cpuid() == 0) {
        g_smp_mutex_in_cs.store(false, std::memory_order_relaxed);
        g_smp_mutex_waiters_ready.store(0, std::memory_order_relaxed);
        g_smp_mutex_counter = 0;
        g_smp_mutex.lock();
    }
    ktest::barrier();

    // Phase 1: force secondary harts to observe the held mutex, queue on
    // WaitQueue, and receive ownership via direct FIFO handoff.
    if(cpuid() == 0) {
        while(g_smp_mutex_waiters_ready.load(std::memory_order_acquire) < harts - 1) {}
        const uint64_t release_at = riscv::csrr(riscv::registers::time) + 1000;
        while(riscv::csrr(riscv::registers::time) < release_at) {}

        KTEST_EXPECT(!g_smp_mutex_in_cs.exchange(true, std::memory_order_acq_rel));
        ++g_smp_mutex_counter;
        g_smp_mutex_in_cs.store(false, std::memory_order_release);
        g_smp_mutex.unlock();
    } else {
        KTEST_EXPECT(!g_smp_mutex.try_lock());
        g_smp_mutex_waiters_ready.fetch_add(1, std::memory_order_release);

        LockGuard guard{g_smp_mutex};
        KTEST_EXPECT(riscv::is_intr_on());
        KTEST_EXPECT_EQ(mycpu()->interrupt_depth, uint32_t{0});
        KTEST_EXPECT(!g_smp_mutex_in_cs.exchange(true, std::memory_order_acq_rel));
        ++g_smp_mutex_counter;
        g_smp_mutex_in_cs.store(false, std::memory_order_release);
    }

    ktest::barrier();

    // Phase 2: repeated multi-hart contention via LockGuard<Mutex>.
    for(uint64_t r = 0; r < ROUNDS_PER_HART; ++r) {
        LockGuard guard{g_smp_mutex};
        KTEST_EXPECT(riscv::is_intr_on());
        KTEST_EXPECT(!g_smp_mutex_in_cs.exchange(true, std::memory_order_acq_rel));
        ++g_smp_mutex_counter;
        const uint64_t spin_until = riscv::csrr(riscv::registers::time) + 200;
        while(riscv::csrr(riscv::registers::time) < spin_until) {}
        g_smp_mutex_in_cs.store(false, std::memory_order_release);
    }

    ktest::barrier();
    if(cpuid() == 0) {
        KTEST_EXPECT_EQ(g_smp_mutex_counter, (1 + ROUNDS_PER_HART) * harts);
    }
}

Spinlock g_smp_lock{};
uint64_t g_smp_counter = 0;

KTEST_SMP_CASE(sync, smp_spinlock_counter) {
    constexpr uint64_t ROUNDS_PER_HART = 2000;

    if(cpuid() == 0) {
        g_smp_counter = 0;
    }
    ktest::barrier();

    for(uint64_t i = 0; i < ROUNDS_PER_HART; ++i) {
        LockGuard guard{g_smp_lock};
        ++g_smp_counter;
    }

    ktest::barrier();
    if(cpuid() == 0) {
        KTEST_EXPECT_EQ(g_smp_counter, ROUNDS_PER_HART * ktest::hart_count());
    }
}

Semaphore g_ping{0};
Semaphore g_pong{0};
uint64_t g_handoff_seq = 0;

KTEST_SMP_CASE(sync, smp_semaphore_handoff) {
    if(ktest::hart_count() < 2) {
        g_ping.release();
        g_ping.acquire();
        return;
    }

    // Stack-allocated WaitQueue::waiter nodes live in the shared root-508
    // kernel-stack window, so cross-hart Semaphore handoff unlinks them
    // directly. Keep round count small because wake() without IPI resumes an
    // idle remote hart on its next timer interrupt.
    constexpr uint64_t ROUNDS = 2;

    if(cpuid() == 0) {
        g_handoff_seq = 0;
    }
    ktest::barrier();

    if(cpuid() == 0) {
        for(uint64_t r = 0; r < ROUNDS; ++r) {
            ++g_handoff_seq;
            g_ping.release();
            g_pong.acquire();
            KTEST_EXPECT_EQ(g_handoff_seq, (r + 1) * 2);
        }
    } else if(cpuid() == 1) {
        for(uint64_t r = 0; r < ROUNDS; ++r) {
            g_ping.acquire();
            KTEST_EXPECT_EQ(g_handoff_seq, r * 2 + 1);
            ++g_handoff_seq;
            g_pong.release();
        }
    }

    ktest::barrier();
}

KTEST_CASE(sync, latch_basic) {
    Latch zero{0};
    KTEST_EXPECT(zero.try_wait());
    zero.wait();
    zero.arrive_and_wait(0);
    KTEST_EXPECT(riscv::is_intr_on());
    KTEST_EXPECT_EQ(mycpu()->interrupt_depth, uint32_t{0});

    Latch counter{3};
    KTEST_EXPECT(!counter.try_wait());
    counter.count_down();
    KTEST_EXPECT(!counter.try_wait());
    counter.count_down(0);
    KTEST_EXPECT(!counter.try_wait());
    counter.count_down(2);
    KTEST_EXPECT(counter.try_wait());
    // Repeated wait() calls return immediately once the latch has opened.
    counter.wait();
    counter.wait();
    KTEST_EXPECT(riscv::is_intr_on());
    KTEST_EXPECT_EQ(mycpu()->interrupt_depth, uint32_t{0});
}

Latch* g_smp_broadcast_latch = nullptr;
Latch* g_smp_arrive_latch = nullptr;
std::atomic<uint64_t> g_smp_latch_payload{0};
std::atomic<size_t> g_smp_latch_waiters_ready{0};

KTEST_SMP_CASE(sync, smp_latch_broadcast_and_arrive) {
    const size_t harts = ktest::hart_count();

    if(cpuid() == 0) {
        g_smp_broadcast_latch = new Latch{1};
        g_smp_arrive_latch = new Latch{harts};
        g_smp_latch_payload.store(0, std::memory_order_relaxed);
        g_smp_latch_waiters_ready.store(0, std::memory_order_relaxed);
    }
    ktest::barrier();

    // Phase 1: one-shot broadcast wake from hart 0 to all secondary harts.
    if(cpuid() == 0) {
        while(g_smp_latch_waiters_ready.load(std::memory_order_acquire) < harts - 1) {}
        const uint64_t release_at = riscv::csrr(riscv::registers::time) + 1000;
        while(riscv::csrr(riscv::registers::time) < release_at) {}

        g_smp_latch_payload.store(0x1a7c'0001ull, std::memory_order_relaxed);
        g_smp_broadcast_latch->count_down();
    } else {
        KTEST_EXPECT(!g_smp_broadcast_latch->try_wait());
        g_smp_latch_waiters_ready.fetch_add(1, std::memory_order_release);

        g_smp_broadcast_latch->wait();
        KTEST_EXPECT(g_smp_broadcast_latch->try_wait());
        KTEST_EXPECT_EQ(g_smp_latch_payload.load(std::memory_order_relaxed), 0x1a7c'0001ull);
        KTEST_EXPECT(riscv::is_intr_on());
        KTEST_EXPECT_EQ(mycpu()->interrupt_depth, uint32_t{0});
    }

    // Phase 2: all harts synchronize via arrive_and_wait().
    g_smp_arrive_latch->arrive_and_wait();
    KTEST_EXPECT(g_smp_arrive_latch->try_wait());
    KTEST_EXPECT(riscv::is_intr_on());
    KTEST_EXPECT_EQ(mycpu()->interrupt_depth, uint32_t{0});

    ktest::barrier();
    if(cpuid() == 0) {
        delete g_smp_broadcast_latch;
        delete g_smp_arrive_latch;
        g_smp_broadcast_latch = nullptr;
        g_smp_arrive_latch = nullptr;
    }
    ktest::barrier();
}

}  // namespace
