#pragma once

// 16550-compatible UART driver: a console sink with an interrupt-driven
// transmit and receive path, a blocking read, and a polled fallback.
//
// The driver is a `CharDevice` (char_device.h) and a `trap::ExternalHandler`
// (trap.h): the console writes into its transmit queue, `read()` blocks on its
// receive queue, and the UART's interrupt drains the first and fills the second.
// It knows nothing about the firmware description; the stride and register width
// arrive as properties decoded by device::Device, and the registers themselves
// are reached through mmio::Region.
//
// Probe returns this object through device::Object. The core discovers its
// CharDevice interface with dyn_cast and installs it as the console; interface
// addresses are adjusted by C++, with no dependency on base-class order.
//
// Two modes share the same register programming:
//
//   - interrupt-driven: `probe()` found an interrupt line, the trap layer
//     registered this object for it and the platform controller enabled it.
//     `write()` copies bytes into a 128-byte transmit queue and returns; the
//     transmitter-empty interrupt moves them into the UART FIFO, and the
//     receive interrupt moves received bytes into a 64-byte receive queue and
//     wakes the thread parked in `read()`. A log line from a trap handler or an
//     interrupt handler is normally a memcpy under a short lock rather than a
//     wait for the serial line; when the queue is already full it falls back to
//     bounded polled draining (see write() below), never to dropping the bytes.
//   - polled: no usable interrupt line (a device with no `interrupts` property,
//     no installed controller, or a full trap table). `write()` polls the line
//     status register and transmits synchronously, exactly as before. Nothing
//     will ever wake a parked thread, so `read()` waits on the hart itself
//     (the timer interrupt returns it to the poll) instead of sleeping. The
//     early firmware console (src/kernel/early) always uses this mode, because
//     it runs before the driver core and before interrupts.
//
// The driver keeps the console working across the handover: both modes write
// through the same registers, and the early object is simply replaced as the
// console sink by the probed one.
//
// Locking: `m_lock` serialises the queues, the interrupt enable register and
// the register accesses the two paths share. It is taken by `write`, `flush`,
// `read`, `enable_interrupts`, `flush_polled` and `handle`; the console lock
// nests outside it (console -> device is the only ordering). `handle()` runs on
// the trap path with interrupts disabled and must not log. It wakes a parked
// reader after releasing `m_lock`, so a switch to that reader cannot happen
// while this hart holds the device lock.
//
// After `enter_panic()` (called by console::enter_panic()), nothing in the
// driver waits for `m_lock` any more: a hart that panicked inside it never
// releases the lock. `write`/`flush` fall back to their lock-free polled forms,
// `handle()` stops touching the device, `read()` reports instead of waiting, and
// the interrupt enable register is cleared so a level-triggered source cannot
// keep claiming the hart. The latch is this driver's own state, which is what
// keeps the dependency one-way -- the driver never asks the console whether a
// panic is in progress.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "char_device.h"
#include "spinlock.h"
#include "trap.h"
#include "drivers/registry.h"
#include "platform/device.h"
#include "platform/mmio.h"

namespace proc {
struct thread;
}

namespace drivers::uart {

// Matched against the device's compatible list, most specific first.
constexpr const inline char* kMatch[] = {"ns16550a", "ns16550", nullptr};

// How many bytes the driver accepts before it has to poll. The capacities are
// powers of two so the ring index is a mask; `write()` drains the transmit
// queue with polled I/O when it is full, which bounds a log burst to
// kTxCapacity extra character times without dropping a byte.
constexpr inline uint32_t kTxCapacity = 128;
constexpr inline uint32_t kRxCapacity = 64;

class Ns16550 final : public device::Implements<CharDevice, trap::ExternalHandler> {
public:
    explicit Ns16550(mmio::Region region) noexcept : m_region(region) {}

    Ns16550(const Ns16550&) = delete;
    Ns16550& operator= (const Ns16550&) = delete;

    // Program the UART. `clock` is the input clock in Hz (0 when the firmware
    // does not describe one), `shift` the register stride as a log2 and `width`
    // the access size in bytes (1 or 4). Interrupts are masked and the queues
    // are emptied; the caller that wants the interrupt path calls
    // enable_interrupts() after the trap layer has accepted the registration.
    void init(uint32_t clock, uint32_t shift, uint32_t width) noexcept;

    // Start using the receive interrupt. Called by probe() only after
    // trap::register_external_handler() succeeded; the transmitter-empty
    // interrupt stays masked until there is something to send, because it is
    // level-triggered on an idle UART and would otherwise fire immediately.
    void enable_interrupts() noexcept;

    // CharDevice: queue output, flush it, or bypass the queue for a panic.
    void write(const char* data, std::size_t count) noexcept override;
    void flush() noexcept override;
    void write_polled(const char* data, std::size_t count) noexcept override;
    void flush_polled() noexcept override;

    // CharDevice: stop waiting for this device's lock from now on.
    void enter_panic() noexcept override;

    // CharDevice: block until a received byte is queued, then take it. Thread
    // context only, on the hart that services this UART (the wake-up is per
    // hart). With a receive interrupt the caller sleeps on the scheduler and the
    // interrupt wakes it; without one the caller waits on the hart and polls the
    // receiver from the timer interrupt. After the panic latch this panics
    // instead of waiting.
    [[nodiscard]] char read() noexcept override;

    // trap::ExternalHandler: service the receiver and the transmitter in one
    // interrupt. `irq` is not needed; this UART owns a single source and the
    // register state says which condition fired. Received bytes are queued under
    // `m_lock`, and a parked reader is woken after the lock is released.
    void handle(uint32_t irq) noexcept override;

    // Bytes dropped because the receive queue was full. Diagnostics only: a
    // reader that keeps up (the shell blocks on read()) never sees one.
    // Atomic because the interrupt handler increments it under `m_lock` while
    // a diagnostics reader may not hold that lock.
    [[nodiscard]] uint32_t rx_overrun_count() const noexcept {
        return m_rx_overrun_count.load(std::memory_order_relaxed);
    }

private:
    // The object is placement-constructed into the driver core's static arena
    // and lives for the rest of the kernel; it is never destroyed. No
    // destructor is needed here: CharDevice's destructor is protected and
    // non-virtual, so a `delete` through the interface does not compile and
    // the static object can never reach the heap's operator delete (see
    // char_device.h).
    [[nodiscard]] uint64_t offset(uint32_t index) const noexcept;
    void write(uint32_t index, uint8_t value) const noexcept;
    [[nodiscard]] uint8_t read(uint32_t index) const noexcept;

    // Program the interrupt enable register and remember its value, so
    // re-enabling the transmitter cannot clear a bit the receiver needs. Once
    // the panic latch is set this only writes zero: a writer that was already in
    // its critical section when enter_panic() ran cannot re-arm a source.
    void set_interrupt_enable(uint8_t bits) noexcept;

    // The body of write() with `m_lock` already held.
    void write_locked(const char* data, std::size_t count) noexcept;

    // Take one queued byte, or report that none is. Never waits. Requires
    // `m_lock`.
    [[nodiscard]] std::optional<char> take_byte_locked() noexcept;

    // Transmit one byte by polling the line status register. No lock: the
    // caller is the polled path, or already owns `m_lock` (or is panicking).
    void putc_polled(char c) noexcept;

    // Move the transmit queue to the FIFO by polling. Requires `m_lock`.
    void drain_transmit_polled() noexcept;

    // Ask for the transmitter-empty interrupt once there is something to
    // send. Requires `m_lock`.
    void start_transmit_locked() noexcept;

    // Move bytes from the receiver FIFO into the receive queue, bounded by
    // the FIFO depth so one interrupt cannot pin the hart. Returns how many bytes
    // it queued, so handle() knows whether a reader has something to wake for.
    // Requires `m_lock`.
    [[nodiscard]] uint32_t service_receive_locked() noexcept;

    // Refill the transmitter FIFO from the queue, or mask the interrupt
    // again when the queue has drained. Requires `m_lock`.
    void service_transmit_locked() noexcept;

    [[nodiscard]] uint32_t transmit_count() const noexcept {
        return m_tx_head - m_tx_tail;
    }

    [[nodiscard]] uint32_t receive_count() const noexcept {
        return m_rx_head - m_rx_tail;
    }

    constexpr static uint32_t TX_MASK = kTxCapacity - 1;
    constexpr static uint32_t RX_MASK = kRxCapacity - 1;
    static_assert((kTxCapacity & (kTxCapacity - 1)) == 0);
    static_assert((kRxCapacity & (kRxCapacity - 1)) == 0);

    mmio::Region m_region{};
    uint32_t m_clock = 0;
    uint32_t m_shift = 0;
    uint32_t m_width = 1;

    // Serialises both queues, the interrupt enable register and the register
    // accesses that the interrupt handler shares with the queueing paths.
    Spinlock m_lock{};

    // Accessed under `m_lock`, except `m_is_interrupt_driven` which is written
    // once by probe() before the console is attached and then only read.
    bool m_is_interrupt_driven = false;
    bool m_tx_interrupt_enabled = false;

    // The interrupt enable mask last programmed into IER. Atomic because
    // enter_panic() masks the device without taking `m_lock` -- the panicking
    // hart may hold it -- while a writer in its critical section can still be
    // updating this value. set_interrupt_enable() re-checks m_panicking after
    // its write, so the panic mask stays the final state in every interleaving.
    std::atomic<uint8_t> m_ier{0};
    uint32_t m_tx_fifo_depth = 1;
    uint32_t m_rx_fifo_depth = 1;

    // Latched by enter_panic() and never cleared. Every path that takes
    // `m_lock` reads it first and gives up instead of waiting, so a hart that
    // panicked inside the driver cannot leave the others parked on the lock.
    std::atomic<bool> m_panicking{false};

    // The thread parked in read(), or nullptr. read() publishes it with
    // interrupts masked just before it sleeps; handle() reads it after queueing
    // bytes and calls shed::wake() on it. Only the reader writes it, and wake()
    // ignores a thread that is not sleeping, so a stale pointer is harmless.
    std::atomic<proc::thread*> m_reader{nullptr};

    std::atomic<uint32_t> m_rx_overrun_count{0};

    // Producer/consumer counters, not indices: `head - tail` is the occupancy
    // and `head & mask` the slot, which also works across the uint32_t wrap
    // because the capacity divides 2^32.
    char m_tx[kTxCapacity]{};
    uint32_t m_tx_head = 0;
    uint32_t m_tx_tail = 0;
    char m_rx[kRxCapacity]{};
    uint32_t m_rx_head = 0;
    uint32_t m_rx_tail = 0;
};

// Private state the driver core reserves for this driver. `Ns16550` holds the
// mmio::Region, the programming parameters, the lock and both queues, so the
// descriptor's priv_size is exactly its size; probe placement-news the
// object into that storage.
constexpr inline uint32_t kPrivSize = sizeof(Ns16550);
static_assert(alignof(Ns16550) <= driver::PRIVATE_STATE_ALIGN);

// Device level: the UART driver takes over the console from the early firmware
// console (src/kernel/early) once the driver core is running. When the device
// describes an interrupt line and the trap layer accepts it, the driver runs
// interrupt-driven; otherwise it stays polled.
//
// The core discovers CharDevice on the returned object and installs it as the
// console. Probe only initialises the device and registers its interrupt line.
device::Object* probe(device::Device& device, void* priv) noexcept;

}  // namespace drivers::uart
