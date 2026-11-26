#pragma once
#include "proc/process.h"

// Per-hart scheduling of kernel threads with cross-hart wake support.
//
// sleep(), schedule() and yield() require a current thread, local interrupts
// disabled and no spinlock held: lock ownership and interrupt nesting belong
// to the hart, not to the thread.
//
// Blocking uses a two-phase handshake so callers do not have to hand a
// synchronization primitive's spinlock to the scheduler:
// 1. Disable local interrupts and take the resource lock.
// 2. Call prepare_sleep() to mark the current thread Blocking, then publish
//    the thread/waiter to the wait structure.
// 3. Release the resource lock (leaving local interrupts disabled) and call
//    sleep().
// If wake() arrives between step 2 and step 3, it flips Blocking back to
// Running under the target hart's lock, and sleep() returns without parking.
namespace sched {

// Bind a newly created thread (ThreadState::Created, hart == nullptr) to the
// current hart and append it to the ready queue in ThreadState::Ready.
void enqueue(proc::thread* thread) noexcept;

// Mark the current non-idle thread Blocking before publishing it to a waker.
// Requires local interrupts disabled (which must stay disabled until sleep()
// returns) and current->state == Running. May be called while holding a
// synchronization primitive's spinlock; does not acquire cpu->lock.
void prepare_sleep() noexcept;

// Complete a two-phase sleep initiated by prepare_sleep().
//
// If a concurrent wake() already flipped Blocking -> Running, returns
// immediately without switching. Otherwise transitions Blocking -> Blocked,
// appends the thread to this hart's sleeping queue, and switches to the next
// ready thread (or idle if none is ready).
//
// Requires local interrupts disabled and no spinlock held.
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

// Wake a thread that is Blocking or Blocked. Safe from any hart and from
// interrupt handlers: acquires the target thread's hart lock.
//
// - Blocking -> Running (early wake consumed by sleep() without parking).
// - Blocked  -> Ready   (removed from target hart's sleeping queue in O(1) and
//                        appended to its ready queue).
// - Other states / nullptr / unbound threads are a no-op.
//
// Does not switch contexts and does not send a cross-hart IPI yet; a thread
// woken on an idle remote hart runs on that hart's next timer or external IRQ.
void wake(proc::thread* thread) noexcept;

// Retire the current non-idle thread and switch to the next ready thread (or
// idle). The retired thread is parked in `mycpu()->dying_thread` (never placed
// on ready/sleeping again) and immediately reclaimed by `proc::reap_dying()`
// once execution lands on the target thread's stack.
//
// Requires local interrupts disabled and no spinlock held. Never returns.
[[noreturn]] void exit_current() noexcept;

}  // namespace sched
