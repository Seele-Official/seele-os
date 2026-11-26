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

// Validate common preconditions for context-switching entry points
// (reschedule, sleep, exit_current) and return the hart's current thread.
proc::thread* require_switch_current(cpu* hart, const char* caller, bool allow_idle) noexcept {
    SEELE_ASSERT(!is_intr_on(), "{}: interruptible", caller);
    SEELE_ASSERT(hart->interrupt_depth == 0, "{}: spinlock", caller);
    proc::thread* const current = hart->current;
    SEELE_ASSERT(current != nullptr, "{}: without a current thread", caller);
    SEELE_ASSERT(allow_idle || current != hart->idle, "{}: called from the idle thread", caller);
    SEELE_ASSERT(current->hart.load(std::memory_order_relaxed) == hart,
                 "{}: current thread does not belong to this hart",
                 caller);
    return current;
}

// Pop the next runnable thread under `hart->lock` (falling back to `idle` when
// `fallback_to_idle` is true), mark it Running, and install it as
// `hart->current`. Returns nullptr only when `fallback_to_idle` is false and
// `ready` is empty.
proc::thread* pick_next_locked(cpu* hart,
                               proc::thread* current,
                               bool fallback_to_idle,
                               const char* caller) noexcept {
    proc::thread* next = hart->ready.pop_front();
    if(next == nullptr) {
        if(!fallback_to_idle) {
            return nullptr;
        }
        next = hart->idle;
    }
    SEELE_ASSERT(next != current, "{}: current thread was already ready", caller);
    SEELE_ASSERT(next->state.load(std::memory_order_acquire) == proc::ThreadState::Ready,
                 "{}: next thread is not ready (state={})",
                 caller,
                 proc::thread_state_name(next->state.load(std::memory_order_relaxed)));

    next->state.store(proc::ThreadState::Running, std::memory_order_release);
    hart->current = next;
    return next;
}

// Switch contexts and immediately reclaim any exited thread left in
// `mycpu()->dying_thread` now that we are on the resumed thread's stack.
void switch_to(proc::thread* current, proc::thread* next) noexcept {
    switch_thread(current, next);
    proc::reap_dying();
}

// Shared path for timer/external preemption and voluntary yield(). Idle stays
// outside the ready queue and only runs when there is no ready work.
void reschedule() noexcept {
    cpu* const hart = mycpu();
    proc::thread* const current = require_switch_current(hart, "sched::schedule()", true);

    while(true) {
        proc::thread* next = nullptr;
        {
            LockGuard guard{hart->lock};

            SEELE_ASSERT(current->state.load(std::memory_order_acquire) ==
                             proc::ThreadState::Running,
                         "sched::schedule(): current thread is not running (state={})",
                         proc::thread_state_name(current->state.load(std::memory_order_relaxed)));

            next = pick_next_locked(hart, current, false, "sched::schedule()");
            // With no other ready thread, keep the current one running rather than
            // giving its hart to idle.
            if(next == nullptr) {
                return;
            }

            current->state.store(proc::ThreadState::Ready, std::memory_order_release);
            if(current != hart->idle) {
                hart->ready.push_back(current);
            }
        }

        switch_to(current, next);

        // If `idle` resumed from an exiting thread whose reap_dying() woke a
        // waiter onto this hart's ready queue, dispatch it immediately instead
        // of returning to idle's wfi() until the next timer tick.
        if(current != hart->idle) {
            return;
        }
    }
}

}  // namespace

void enqueue(proc::thread* thread) noexcept {
    SEELE_ASSERT(thread != nullptr, "sched::enqueue(nullptr)");

    cpu* const hart = mycpu();
    LockGuard guard{hart->lock};

    SEELE_ASSERT(thread != hart->idle, "sched::enqueue(): idle thread never enters ready");
    SEELE_ASSERT(thread->state.load(std::memory_order_acquire) == proc::ThreadState::Created,
                 "sched::enqueue(): thread is not newly created (state={})",
                 proc::thread_state_name(thread->state.load(std::memory_order_relaxed)));
    SEELE_ASSERT(thread->hart.load(std::memory_order_acquire) == nullptr,
                 "sched::enqueue(): thread is already bound to a hart");

    thread->hart.store(hart, std::memory_order_release);
    thread->state.store(proc::ThreadState::Ready, std::memory_order_release);
    hart->ready.push_back(thread);
}

void prepare_sleep() noexcept {
    cpu* const hart = mycpu();
    SEELE_ASSERT(!is_intr_on(), "sched::prepare_sleep() with interrupts enabled");
    proc::thread* const current = hart->current;
    SEELE_ASSERT(current != nullptr, "sched::prepare_sleep() without a current thread");
    SEELE_ASSERT(current != hart->idle, "sched::prepare_sleep() from the idle thread");
    SEELE_ASSERT(current->hart.load(std::memory_order_relaxed) == hart,
                 "sched::prepare_sleep(): current thread does not belong to this hart");
    SEELE_ASSERT(current->state.load(std::memory_order_relaxed) == proc::ThreadState::Running,
                 "sched::prepare_sleep(): current thread is not running (state={})",
                 proc::thread_state_name(current->state.load(std::memory_order_relaxed)));

    current->state.store(proc::ThreadState::Blocking, std::memory_order_release);
}

void sleep() noexcept {
    cpu* const hart = mycpu();
    proc::thread* const current = require_switch_current(hart, "sched::sleep()", false);

    proc::thread* next = nullptr;
    {
        LockGuard guard{hart->lock};

        const auto state = current->state.load(std::memory_order_acquire);
        if(state == proc::ThreadState::Running) {
            // A concurrent wake() arrived after prepare_sleep() and flipped
            // Blocking back to Running; consume it without parking.
            return;
        }
        SEELE_ASSERT(state == proc::ThreadState::Blocking,
                     "sched::sleep() requires prepare_sleep() (state={})",
                     proc::thread_state_name(state));

        next = pick_next_locked(hart, current, true, "sched::sleep()");
        current->state.store(proc::ThreadState::Blocked, std::memory_order_release);
        hart->sleeping.push_back(current);
    }

    switch_to(current, next);
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

    cpu* const target_cpu = thread->hart.load(std::memory_order_acquire);
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
    cpu* const hart = mycpu();
    proc::thread* const current = require_switch_current(hart, "sched::exit_current()", false);

    proc::thread* next = nullptr;
    {
        LockGuard guard{hart->lock};

        SEELE_ASSERT(current->state.load(std::memory_order_acquire) == proc::ThreadState::Running,
                     "sched::exit_current(): current thread is not running (state={})",
                     proc::thread_state_name(current->state.load(std::memory_order_relaxed)));
        SEELE_ASSERT(hart->dying_thread == nullptr,
                     "sched::exit_current(): previous dying thread was not reaped");

        next = pick_next_locked(hart, current, true, "sched::exit_current()");
        hart->dying_thread = current;
    }

    switch_thread(current, next);
    SEELE_UNREACHABLE();
}

}  // namespace sched
