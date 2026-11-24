#include "sched/schedule.h"

#include "hart.h"
#include "panic.h"
#include "riscv.h"
#include "concurrency/lock_guard.h"
#include "sched/switch.h"

namespace sched {
using namespace riscv;
using namespace riscv::registers;

namespace {
// Shared policy for preemption and voluntary sleep/yield. A sleeping thread
// waits for wake(); otherwise the outgoing non-idle thread remains runnable.
// Idle stays outside the ready queue and only runs when there is no ready work.
// Callers disable local interrupts and hold no spinlock across the switch.
template <bool sleep>
void helper() noexcept {
    auto* cpu = mycpu();
    SEELE_ASSERT(!is_intr_on(), "interruptible");
    SEELE_ASSERT(cpu->interrupt_depth == 0, "spinlock");
    auto* current = cpu->current;

    {
        LockGuard guard{cpu->lock};

        proc::thread* next = cpu->ready.pop_front();

        if constexpr(sleep) {
            // The idle thread is what the hart falls back to; it never sleeps,
            // or the hart would have no thread to switch to.
            SEELE_ASSERT(current != cpu->idle, "sched::sleep() from the idle thread");
            // A running thread must not also be the next ready thread. This
            // check does not detect lost wakes: callers must prevent a wake
            // between checking their condition and entering the sleeping list.
            SEELE_ASSERT(next != current, "sched::sleep(): a wake() arrived too early");

            if(next == nullptr) {
                next = cpu->idle;
            }
            cpu->sleeping.push_back(current);
        } else {
            // With no other ready thread, keep the current one running rather
            // than giving its hart to idle. Only non-idle threads are requeued.
            if(next == nullptr) {
                return;
            }
            if(current != cpu->idle) {
                cpu->ready.push_back(current);
            }
        }
        cpu->current = next;
    }

    switch_thread(current, cpu->current);
}
}  // namespace

void sleep() noexcept {
    helper<true>();
}

void schedule() noexcept {
    helper<false>();
}

void yield() noexcept {
    helper<false>();
}

void wake(proc::thread* thread) noexcept {
    auto* cpu = mycpu();
    LockGuard guard{cpu->lock};
    for(proc::thread& waiting: cpu->sleeping) {
        if(&waiting == thread) {
            cpu->sleeping.erase(thread);
            cpu->ready.push_back(thread);
            return;
        }
    }
}

}  // namespace sched
