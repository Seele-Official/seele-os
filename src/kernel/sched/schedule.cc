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

// Shared path for timer/external preemption and voluntary yield(). Idle stays
// outside the ready queue and only runs when there is no ready work.
void reschedule() noexcept {
    auto* cpu = mycpu();
    SEELE_ASSERT(!is_intr_on(), "interruptible");
    SEELE_ASSERT(cpu->interrupt_depth == 0, "spinlock");
    auto* current = cpu->current;
    SEELE_ASSERT(current != nullptr, "sched::schedule() without a current thread");
    SEELE_ASSERT(current->hart.load(std::memory_order_relaxed) == cpu,
                 "sched::schedule(): current thread does not belong to this hart");

    proc::thread* next = nullptr;
    {
        LockGuard guard{cpu->lock};

        SEELE_ASSERT(current->state.load(std::memory_order_acquire) == proc::ThreadState::Running,
                     "sched::schedule(): current thread is not running (state={})",
                     proc::thread_state_name(current->state.load(std::memory_order_relaxed)));

        next = cpu->ready.pop_front();
        // With no other ready thread, keep the current one running rather than
        // giving its hart to idle.
        if(next == nullptr) {
            return;
        }
        SEELE_ASSERT(next != current, "sched::schedule(): current thread was already ready");
        SEELE_ASSERT(next->state.load(std::memory_order_acquire) == proc::ThreadState::Ready,
                     "sched::schedule(): next thread is not ready (state={})",
                     proc::thread_state_name(next->state.load(std::memory_order_relaxed)));

        current->state.store(proc::ThreadState::Ready, std::memory_order_release);
        if(current != cpu->idle) {
            cpu->ready.push_back(current);
        }
        next->state.store(proc::ThreadState::Running, std::memory_order_release);
        cpu->current = next;
    }

    switch_thread(current, next);
}

}  // namespace

void enqueue(proc::thread* thread) noexcept {
    SEELE_ASSERT(thread != nullptr, "sched::enqueue(nullptr)");

    auto* cpu = mycpu();
    LockGuard guard{cpu->lock};

    SEELE_ASSERT(thread != cpu->idle, "sched::enqueue(): idle thread never enters ready");
    SEELE_ASSERT(thread->state.load(std::memory_order_acquire) == proc::ThreadState::Created,
                 "sched::enqueue(): thread is not newly created (state={})",
                 proc::thread_state_name(thread->state.load(std::memory_order_relaxed)));
    SEELE_ASSERT(thread->hart.load(std::memory_order_acquire) == nullptr,
                 "sched::enqueue(): thread is already bound to a hart");

    thread->hart.store(cpu, std::memory_order_release);
    thread->state.store(proc::ThreadState::Ready, std::memory_order_release);
    cpu->ready.push_back(thread);
}

void prepare_sleep() noexcept {
    auto* cpu = mycpu();
    SEELE_ASSERT(!is_intr_on(), "sched::prepare_sleep() with interrupts enabled");
    auto* current = cpu->current;
    SEELE_ASSERT(current != nullptr, "sched::prepare_sleep() without a current thread");
    SEELE_ASSERT(current != cpu->idle, "sched::prepare_sleep() from the idle thread");
    SEELE_ASSERT(current->hart.load(std::memory_order_relaxed) == cpu,
                 "sched::prepare_sleep(): current thread does not belong to this hart");
    SEELE_ASSERT(current->state.load(std::memory_order_relaxed) == proc::ThreadState::Running,
                 "sched::prepare_sleep(): current thread is not running (state={})",
                 proc::thread_state_name(current->state.load(std::memory_order_relaxed)));

    current->state.store(proc::ThreadState::Blocking, std::memory_order_release);
}

void sleep() noexcept {
    auto* cpu = mycpu();
    SEELE_ASSERT(!is_intr_on(), "interruptible");
    SEELE_ASSERT(cpu->interrupt_depth == 0, "spinlock");
    auto* current = cpu->current;
    SEELE_ASSERT(current != nullptr, "sched::sleep() without a current thread");
    SEELE_ASSERT(current != cpu->idle, "sched::sleep() from the idle thread");
    SEELE_ASSERT(current->hart.load(std::memory_order_relaxed) == cpu,
                 "sched::sleep(): current thread does not belong to this hart");

    proc::thread* next = nullptr;
    {
        LockGuard guard{cpu->lock};

        const auto state = current->state.load(std::memory_order_acquire);
        if(state == proc::ThreadState::Running) {
            // A concurrent wake() arrived after prepare_sleep() and flipped
            // Blocking back to Running; consume it without parking.
            return;
        }
        SEELE_ASSERT(state == proc::ThreadState::Blocking,
                     "sched::sleep() requires prepare_sleep() (state={})",
                     proc::thread_state_name(state));

        next = cpu->ready.pop_front();
        SEELE_ASSERT(next != current, "sched::sleep(): current thread was already ready");
        if(next == nullptr) {
            next = cpu->idle;
        }
        SEELE_ASSERT(next->state.load(std::memory_order_acquire) == proc::ThreadState::Ready,
                     "sched::sleep(): next thread is not ready (state={})",
                     proc::thread_state_name(next->state.load(std::memory_order_relaxed)));

        current->state.store(proc::ThreadState::Blocked, std::memory_order_release);
        cpu->sleeping.push_back(current);
        next->state.store(proc::ThreadState::Running, std::memory_order_release);
        cpu->current = next;
    }

    switch_thread(current, next);
}

void schedule() noexcept {
    reschedule();
}

void yield() noexcept {
    reschedule();
}

void wake(proc::thread* thread) noexcept {
    if(thread == nullptr) {
        return;
    }

    cpu* target_cpu = thread->hart.load(std::memory_order_acquire);
    if(target_cpu == nullptr) {
        return;
    }

    LockGuard guard{target_cpu->lock};

    const auto state = thread->state.load(std::memory_order_acquire);
    if(state == proc::ThreadState::Blocking) {
        // Early wake: the thread has not entered cpu->sleeping yet. Flipping
        // Blocking -> Running tells sleep() to return immediately.
        thread->state.store(proc::ThreadState::Running, std::memory_order_release);
        return;
    }

    if(state == proc::ThreadState::Blocked) {
        // Under target_cpu->lock, Blocked holds iff the thread is linked in
        // target_cpu->sleeping, so unlinking is O(1).
        target_cpu->sleeping.erase(thread);
        thread->state.store(proc::ThreadState::Ready, std::memory_order_release);
        target_cpu->ready.push_back(thread);
    }
}

[[noreturn]] void exit_current() noexcept {
    auto* cpu = mycpu();
    SEELE_ASSERT(!is_intr_on(), "interruptible");
    SEELE_ASSERT(cpu->interrupt_depth == 0, "spinlock");
    auto* current = cpu->current;
    SEELE_ASSERT(current != nullptr, "sched::exit_current() without a current thread");
    SEELE_ASSERT(current != cpu->idle, "sched::exit_current() from the idle thread");
    SEELE_ASSERT(current->hart.load(std::memory_order_relaxed) == cpu,
                 "sched::exit_current(): current thread does not belong to this hart");

    proc::thread* next = nullptr;
    {
        LockGuard guard{cpu->lock};

        SEELE_ASSERT(current->state.load(std::memory_order_acquire) == proc::ThreadState::Running,
                     "sched::exit_current(): current thread is not running (state={})",
                     proc::thread_state_name(current->state.load(std::memory_order_relaxed)));

        next = cpu->ready.pop_front();
        SEELE_ASSERT(next != current, "sched::exit_current(): current thread was already ready");
        if(next == nullptr) {
            next = cpu->idle;
        }
        SEELE_ASSERT(next->state.load(std::memory_order_acquire) == proc::ThreadState::Ready,
                     "sched::exit_current(): next thread is not ready (state={})",
                     proc::thread_state_name(next->state.load(std::memory_order_relaxed)));

        current->state.store(proc::ThreadState::Exited, std::memory_order_release);
        next->state.store(proc::ThreadState::Running, std::memory_order_release);
        cpu->current = next;
    }

    switch_thread(current, next);
    SEELE_UNREACHABLE();
}

}  // namespace sched
