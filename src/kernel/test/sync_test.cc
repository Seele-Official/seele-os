#include <cstddef>
#include <cstdint>

#include "hart.h"
#include "riscv.h"
#include "concurrency/lock_guard.h"
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

}  // namespace
