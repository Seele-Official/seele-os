#pragma once

// Output sink and interactive character-device interfaces shared by the console
// and its backends.
//
// Output and input are split cleanly into two layers:
//   - `ConsoleSink`: output + panic hooks only. Implemented by polled early
//     boot consoles (16550 UART, future OpenSBI debug console) that run before
//     interrupts, locks, or the driver core exist.
//   - `CharDevice`: extends `ConsoleSink` with blocking `read()`. Implemented
//     by full drivers (e.g. Ns16550) discovered via `device::dyn_cast` and
//     attached through `console::attach()`.
//
// The console owns line framing, lock ordering, and normal-vs-panic routing;
// backends own hardware access, buffering, and (for CharDevice) blocking waits.
// Backends never call back into the console.

#include <cstddef>

class ConsoleSink {
public:
    // Accept `count` bytes for transmission and return without scheduling.
    // Callable from normal code, traps, interrupts, and early boot.
    virtual void write(const char* data, std::size_t count) noexcept = 0;

    // Drain accepted bytes and wait until the transmitter is quiet. May take
    // the device lock, so it must not be called from that device's own IRQ
    // handler.
    virtual void flush() noexcept = 0;

    // Lock-free emergency output for panic(). Never takes locks or touches
    // shared queues that a halted hart may have left locked.
    virtual void write_polled(const char* data, std::size_t count) noexcept = 0;

    // Best-effort drain for panic(): never blocks on a held lock.
    virtual void flush_polled() noexcept = 0;

    // Switch this backend permanently to its lock-free panic path.
    virtual void enter_panic() noexcept = 0;

protected:
    // Backends live in static storage or the driver arena and are never freed.
    ~ConsoleSink() = default;
};

class CharDevice : public ConsoleSink {
public:
    // Block until at least one input byte is available, then return it. Thread
    // context only (interrupts enabled, no spinlock held).
    [[nodiscard]] virtual char read() noexcept = 0;

protected:
    ~CharDevice() = default;
};
