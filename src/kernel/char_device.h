#pragma once

// The character-device interface a console and its driver share.
//
// The console is the policy layer: it formats output, decides how output waits
// and is what the shell and the LOG_* macros call. A driver implements the
// interface below and owns the hardware details: the transmit queue, the receive
// queue and -- for input -- the wait for a byte.
//
// The two directions have different owners, which is why one interface covers
// both, and the split is drawn at the byte queue. The console owns line framing,
// ordering and the choice between the queued and the polled path, because it has
// to keep working from contexts that may not wait (trap handlers, panic). The
// driver owns the byte queue behind that choice: how much it buffers, and how it
// keeps write() bounded when the queue is full, is a property of the hardware
// and its interrupt. Input has no policy to split: only the driver
// knows whether it has a receive interrupt, where received bytes are queued and
// how to wake a thread when one arrives, so the blocking read() is the driver's
// method and the console only forwards to it. A driver never calls back into the
// console: it answers read(), write() and the panic hooks, and that is the whole
// contract.
//
// Which device a driver binds is declared, not exported: the driver table names
// driver::Type::Console (drivers/registry.h) and the core installs the object
// with console::attach(). The interface must be the first base class of the
// driver's state block, so the core can recover the CharDevice from the
// driver-agnostic `void*` with a single cast; the driver checks that layout when
// it binds.

#include <cstddef>
#include <cstdint>

// A character device: the sink the console writes through and the source its
// read() blocks on. The kernel's current one is a UART.
//
// The console owns no device of its own, so it does not need to know which
// driver (or which firmware description) produced this object.
//
// The output methods are one policy split: `write`/`flush` are the normal
// buffered path, `write_polled`/`flush_polled` the panic path. Neither may
// schedule; the difference is what they are allowed to lock.
class CharDevice {
public:
    // Accept `count` bytes for transmission and return. A sink with an
    // interrupt path copies them into its transmit queue and lets the
    // interrupt drain it; a sink without one writes them out under polled I/O.
    // When the queue is full the sink drains it by polling the transmitter
    // before accepting more, so the call is bounded by the queue size.
    //
    // Callable from every context -- plain kernel code, trap handler, interrupt
    // handler, boot code with interrupts disabled -- because it takes only
    // locks that are released before it returns and never sleeps.
    virtual void write(const char* data, std::size_t count) noexcept = 0;

    // Transmit everything write() has accepted and wait until the device is
    // quiet again (queue and shift register empty), by polling. Takes the
    // device's own lock, so it must not be called from that device's interrupt
    // handler. Used to order output with a handover or with a terminal.
    virtual void flush() noexcept = 0;

    // Emergency output for panic(): writes the bytes straight to the hardware
    // with polled I/O, takes no lock at all and never touches a queue another
    // hart could be using. Bytes that write() accepted before the panic are not
    // drained here; console::enter_panic() flushes those once, before the first
    // panic line. A concurrent writer on another hart can interleave with these
    // bytes -- output fidelity, never memory safety, is affected.
    virtual void write_polled(const char* data, std::size_t count) noexcept = 0;

    // Best-effort drain for panic(): like flush(), but never waits for a lock.
    // When the device lock is not free (this hart already holds it, or another
    // hart that may not run again holds it), the queued bytes stay where they
    // are and the function returns.
    virtual void flush_polled() noexcept = 0;

    // Switch this device to the panic path. Called once by console::enter_panic()
    // on the attached device, before the first panic line is written.
    //
    // From then on the console calls only write_polled()/flush_polled(), and the
    // other methods must stop waiting for anything: a hart that panicked while
    // holding the device's lock never releases it, so a second hart parked on
    // that lock would hang instead of printing. The flag this sets lives in the
    // device, which is what keeps the dependency one-way: the console tells the
    // device about the panic, the device never asks the console about it.
    virtual void enter_panic() noexcept = 0;

    // Block until at least one byte is available, then return it. Thread
    // context only: a current thread, interrupts enabled and no spinlock held.
    //
    // The wait is the driver's. It parks the calling thread on the scheduler's
    // sleeping list -- with interrupts masked from the empty check to the
    // switch, so the receive interrupt cannot wake a thread that has not slept
    // yet -- and its receive interrupt wakes it again through shed::wake(). A
    // device with no receive interrupt has nothing that can wake a parked
    // thread, so it waits on the hart instead; which of the two applies is the
    // driver's decision, made from its own configuration.
    [[nodiscard]] virtual char read() noexcept = 0;

protected:
    // A device lives in static storage or the driver core's arena and is never
    // destroyed, so there is deliberately no deallocation path. A protected,
    // non-virtual destructor states that (C++ Core Guidelines C.35): a `delete`
    // through a CharDevice* does not compile, so such an object can never be
    // handed to the kernel heap's operator delete. Do not make this destructor
    // public or virtual without changing that: a virtual destructor would make
    // `delete` on the interface compile and deallocate memory the heap never
    // allocated.
    ~CharDevice() = default;
};
