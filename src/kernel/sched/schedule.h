#pragma once
#include "proc/process.h"

// Hart-local scheduling of kernel threads. sleep(), schedule() and yield()
// require a current thread, local interrupts disabled and no spinlock held:
// lock ownership and interrupt nesting belong to the hart, not to the thread.
// wake() only makes a sleeping thread ready; it does not switch contexts.
namespace sched {

// Park the current non-idle thread until wake() makes it ready on this hart.
// Run another ready thread, or fall back to idle if none exists.
//
// The caller must prevent a wake between checking its wait condition and
// parking: wake() does not remember requests for a thread not yet sleeping.
// The UART reader closes this window by keeping local interrupts disabled
// through its condition check and sleep, releasing its device lock first.
void sleep() noexcept;

// Give the hart to the next ready thread and put the current one back at the tail
// of the run queue. Called from the trap path for every timer tick and for every
// serviced external interrupt; returns immediately when nothing is ready. The
// idle thread is not one of the candidates: a hart runs idle only when the ready
// list is empty, so a running thread is never preempted in favour of it.
void schedule() noexcept;

// Hand the hart to another ready thread, keeping this one runnable. Nothing calls
// it yet; it is the scheduling point a long-running kernel loop would use.
void yield() noexcept;

// Make a thread that slept in sleep() runnable again. The caller must run on the
// hart the thread was parked on, because the sleeping list is per hart; safe from
// an interrupt handler and from the trap path there (it takes the hart's
// run-queue lock, which is also what keeps it from racing a switch). A thread
// that is not sleeping is left alone, so a spurious wake-up is a no-op rather
// than a corruption.
void wake(proc::thread* thread) noexcept;

}  // namespace sched
