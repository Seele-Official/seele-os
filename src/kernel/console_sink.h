#pragma once

// Synchronous kernel output sink interface shared by the console and its
// backends (early boot consoles and full UART drivers).
//
// `ConsoleSink` serves the two non-scheduling kernel output paths:
//   - Normal kernel logging/printing (`write_sync`): serialized by the console
//     spinlock, may acquire the backend's own spinlock, never sleeps.
//   - Panic/emergency output (`enter_panic` + `write_emergency`): completely
//     lock-free hardware bypass so crash diagnostics cannot deadlock.
//
// Interactive, schedulable character-stream I/O (`read`/`write` for syscalls)
// is an orthogonal interface (`CharDevice`, char_device.h) rather than a
// subtype of `ConsoleSink`. A driver such as Ns16550 implements both via
// `device::Implements<ConsoleSink, CharDevice, ...>`.

#include <string_view>

class ConsoleSink {
public:
    // Synchronously transmit `data` to hardware without scheduling.
    // Callable from early boot, normal threads, traps, and non-reentrant IRQs;
    // may take the device spinlock (lock order: console -> device). Backends
    // that also buffer async CharDevice TX drain pending buffered bytes first
    // so causal output order is preserved.
    virtual void write_sync(std::string_view data) noexcept = 0;

    // Drain any buffered TX and wait until the hardware transmitter is quiet.
    // Non-scheduling; may take the device spinlock (must not be called from
    // this device's own IRQ handler).
    virtual void flush() noexcept = 0;

    // Lock-free emergency output for panic(). Never takes locks or touches
    // shared queues that another hart may have left locked or inconsistent.
    virtual void write_emergency(std::string_view data) noexcept = 0;

    // Mask device interrupts and latch panic state without taking locks so
    // level-triggered IRQ storms or concurrent writers cannot interfere with
    // crash diagnostics.
    virtual void enter_panic() noexcept = 0;

protected:
    // Backends live in static storage or the driver arena and are never freed.
    ~ConsoleSink() = default;
};
