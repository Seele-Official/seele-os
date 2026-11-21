// 16550-compatible UART driver (drivers/uart/ns16550.h).

#include "drivers/uart/ns16550.h"

#include <cstddef>
#include <cstdint>
#include <new>
#include <optional>

#include "console.h"
#include "hart.h"
#include "lock_guard.h"
#include "panic.h"
#include "riscv.h"
#include "trap.h"
#include "shed/schedule.h"

namespace drivers::uart {
namespace {

// Register numbers. The driver never forms the addresses itself: `reg-shift`
// turns a register number into a byte offset inside the mmio::Region.
constexpr uint32_t RBR = 0;  // receive holding register (for input bytes)
constexpr uint32_t THR = 0;  // transmit holding register (for output bytes)
constexpr uint32_t IER = 1;  // interrupt enable register

constexpr uint32_t DLL = 0;  // divisor latch low (for baud rate divisor)
constexpr uint32_t DLM = 1;  // divisor latch high (for baud rate divisor)

constexpr uint32_t IIR = 2;  // interrupt identification register (read side)
constexpr uint32_t FCR = 2;  // FIFO control register (write side)
constexpr uint32_t LCR = 3;  // line control register
constexpr uint32_t LSR = 5;  // line status register

constexpr uint8_t FCR_FIFO_ENABLE(1 << 0);
constexpr uint8_t FCR_FIFO_CLEAR(3 << 1);  // clear the content of the two FIFOs
constexpr uint8_t LCR_EIGHT_BITS(3 << 0);
constexpr uint8_t LCR_BAUD_LATCH(1 << 7);  // special mode to set baud rate
constexpr uint8_t LSR_TX_IDLE(1 << 5);     // THR can accept another character to send
constexpr uint8_t LSR_TX_EMPTY(1 << 6);    // transmitter FIFO and shift register are empty
constexpr uint8_t LSR_RX_READY(1 << 0);    // RBR holds a received character

// Interrupt enable bits. Only these two are used: the receiver must interrupt
// to fill the receive queue, and the transmitter interrupts while there is
// queued output. The receiver line-status interrupt is deliberately left off:
// this driver does not reject bytes with errors, and every LSR read in the
// service path clears the error bits it would report.
constexpr uint8_t IER_RX_READY(1 << 0);
constexpr uint8_t IER_TX_EMPTY(1 << 1);

// IIR[7:6] report whether the FIFO has been enabled. The driver uses it to
// decide how many bytes one transmitter interrupt may load: a part with no
// FIFO accepts one byte, a 16550A takes the whole 16-byte FIFO.
constexpr uint8_t IIR_FIFO_MASK(3 << 6);
constexpr uint8_t IIR_FIFO_ENABLED(3 << 6);
constexpr uint32_t FIFO_DEPTH = 16;

// Priority the source is enabled with at the platform controller. Any non-zero
// priority enables the line; the kernel has no other console source to rank it
// against.
constexpr uint32_t IRQ_PRIORITY = 1;

// The line rate the driver programs. A build parameter (-DSEELE_UART_BAUD): the
// baud rate is a property of the board's console wiring, not of the 16550
// register map, and CMake rejects a rate the 16-bit divisor cannot reach from
// any input clock.
#ifndef SEELE_UART_BAUD
#error "SEELE_UART_BAUD is not defined: build the kernel through CMake (see CMakeLists.txt)"
#endif
constexpr uint32_t BAUD = SEELE_UART_BAUD;

// Divisor the LCR-bank needs for BAUD from `clock`. A zero or too small
// clock cannot produce a sane divisor, so it falls back to 1 (the slowest
// rate) rather than dividing by zero.
[[nodiscard]] constexpr uint16_t divisor_for(uint32_t clock) noexcept {
    if(clock == 0) {
        return 1;
    }

    const uint32_t divisor = clock / (16u * BAUD);
    if(divisor == 0) {
        return 1;
    }
    if(divisor > 0xffffu) {
        return 0xffff;
    }
    return static_cast<uint16_t>(divisor);
}

}  // namespace

void Ns16550::init(uint32_t clock, uint32_t shift, uint32_t width) noexcept {
    m_clock = clock;
    m_shift = shift;
    m_width = width;

    m_tx_head = 0;
    m_tx_tail = 0;
    m_rx_head = 0;
    m_rx_tail = 0;
    m_tx_interrupt_enabled = false;
    m_is_interrupt_driven = false;
    m_rx_overrun_count.store(0, std::memory_order_relaxed);

    // IER: mask every interrupt. The driver starts polled; enable_interrupts()
    // turns the receiver on once the trap layer has accepted the registration.
    set_interrupt_enable(0);

    // LCR: enable DLAB to set baud rate
    write(LCR, LCR_BAUD_LATCH);

    const uint16_t divisor = divisor_for(m_clock);
    write(DLL, static_cast<uint8_t>(divisor & 0xffu));         // set low byte of divisor
    write(DLM, static_cast<uint8_t>((divisor >> 8) & 0xffu));  // set high byte of divisor

    // LCR: 8 data bits, no parity, 1 stop bit
    write(LCR, LCR_EIGHT_BITS);
    // FCR: enable and clear FIFOs
    write(FCR, static_cast<uint8_t>(FCR_FIFO_ENABLE | FCR_FIFO_CLEAR));

    // IIR tells one-interrupt-at-a-time paths how much room the transmitter
    // has. Reading it here is safe: no interrupt is enabled yet.
    const uint8_t iir = read(IIR);
    const bool has_fifo = (iir & IIR_FIFO_MASK) == IIR_FIFO_ENABLED;
    m_tx_fifo_depth = has_fifo ? FIFO_DEPTH : 1;
    m_rx_fifo_depth = has_fifo ? FIFO_DEPTH : 1;
}

void Ns16550::enable_interrupts() noexcept {
    LockGuard guard{m_lock};
    if(m_is_interrupt_driven) {
        return;
    }
    m_is_interrupt_driven = true;
    // Receiver only. The transmitter is level-triggered on an idle UART, so its
    // interrupt is enabled by start_transmit_locked() when there is a byte and
    // masked again by service_transmit_locked() when the queue drains. OR the
    // bit in instead of overwriting: a later probe retry or a driver that turns
    // the transmitter on first must not lose it.
    set_interrupt_enable(m_ier | IER_RX_READY);
}

void Ns16550::write(const char* data, std::size_t count) noexcept {
    if(data == nullptr || count == 0) {
        return;
    }

    // A hart that panicked inside this driver stops holding `m_lock` forever.
    // Waiting for it would park every other writer with interrupts off, so the
    // panic latch turns output into the lock-free path instead.
    while(true) {
        if(m_panicking.load(std::memory_order_acquire)) {
            write_polled(data, count);
            return;
        }

        TryLockGuard guard{m_lock};
        if(guard.locked()) {
            write_locked(data, count);
            return;
        }

        // try_lock reports a lock this hart already owns as busy, so a write from
        // inside the driver's own critical section would spin here forever.
        // That is a driver bug, not contention: report it.
        SEELE_ASSERT(!m_lock.owned_by_current_hart(), "uart: recursive write");
    }
}

void Ns16550::write_locked(const char* data, std::size_t count) noexcept {
    if(!m_is_interrupt_driven) {
        for(std::size_t i = 0; i < count; ++i) {
            putc_polled(data[i]);
        }
        return;
    }

    for(std::size_t i = 0; i < count; ++i) {
        // The queue is full: transmit what is already queued with polled I/O.
        // This is the backpressure policy -- a log storm becomes slower output
        // rather than lost bytes, and the wait is bounded by kTxCapacity.
        while(transmit_count() == kTxCapacity) {
            drain_transmit_polled();
        }
        m_tx[m_tx_head & TX_MASK] = data[i];
        ++m_tx_head;
    }
    start_transmit_locked();
}

void Ns16550::flush() noexcept {
    while(true) {
        if(m_panicking.load(std::memory_order_acquire)) {
            flush_polled();
            return;
        }

        TryLockGuard guard{m_lock};
        if(guard.locked()) {
            if(m_is_interrupt_driven) {
                drain_transmit_polled();
            }

            // Wait for the shift register too, not just the queue: `write()`
            // returns once the bytes are accepted, and in polled mode they are
            // accepted into the THR/FIFO rather than shifted out. TEMT is what
            // makes "quiet" mean quiet in both modes.
            while((read(LSR) & LSR_TX_EMPTY) == 0) {}
            return;
        }
        SEELE_ASSERT(!m_lock.owned_by_current_hart(), "uart: recursive flush");
    }
}

void Ns16550::write_polled(const char* data, std::size_t count) noexcept {
    if(data == nullptr || count == 0) {
        return;
    }
    // The panic path: no lock, no queue. A concurrent flush on another hart can
    // interleave with these bytes, but the hardware write itself is safe.
    for(std::size_t i = 0; i < count; ++i) {
        putc_polled(data[i]);
    }
}

void Ns16550::flush_polled() noexcept {
    // The lock may be held by this hart (a panic from inside the driver) or by
    // a hart that will not run again. Either way, spinning here would hang, so
    // the queued bytes are left to the next reader instead.
    TryLockGuard guard{m_lock};
    if(!guard.locked()) {
        return;
    }
    if(m_is_interrupt_driven) {
        drain_transmit_polled();
    }
}

void Ns16550::enter_panic() noexcept {
    // Latch first: every path that takes `m_lock` reads it and gives up instead
    // of waiting, and handle() stops touching the device at all.
    m_panicking.store(true, std::memory_order_release);

    // Then mask the sources. An enabled receiver is level-triggered, so leaving
    // it on would make the controller re-claim the line as fast as the trap path
    // can complete it -- a storm on the hart that still takes interrupts, for as
    // long as the kernel stays stopped. The register write is lock-free on
    // purpose: the panicking hart may be the one holding `m_lock`, which is the
    // same rule write_polled() follows.
    m_ier = 0;
    write(IER, 0);
}

std::optional<char> Ns16550::take_byte_locked() noexcept {
    if(!m_is_interrupt_driven) {
        if((read(LSR) & LSR_RX_READY) == 0) {
            return std::nullopt;
        }
        return static_cast<char>(read(RBR));
    }

    // A byte already in the queue is served first: servicing the FIFO while
    // the queue is full would drop a byte that the caller is about to free
    // space for.
    if(receive_count() != 0) {
        const char c = m_rx[m_rx_tail & RX_MASK];
        ++m_rx_tail;
        return c;
    }

    // The receive interrupt may still be pending (interrupts were off while the
    // caller polled). Moving the FIFO into the queue here is harmless and keeps
    // the reader working even if a delivery was lost; the parked reader is woken
    // by handle(), not from here.
    (void)service_receive_locked();
    if(receive_count() == 0) {
        return std::nullopt;
    }
    const char c = m_rx[m_rx_tail & RX_MASK];
    ++m_rx_tail;
    return c;
}

char Ns16550::read() noexcept {
    const cpu* hart = mycpu();
    SEELE_ASSERT(hart->current != nullptr, "uart: read() from a context without a thread");
    SEELE_ASSERT(hart->interrupt_depth == 0, "uart: read() while holding a spinlock");
    SEELE_ASSERT((riscv::csrr(riscv::registers::sstatus) & riscv::csr::sstatus::SIE) != 0,
                 "uart: read() with interrupts disabled");

    // This reader must be on the hart that services the UART: shed::sleep() parks
    // it on THIS hart's sleeping list and wake() only scans that list, so a
    // reader on another hart would publish m_reader and then never be woken.
    SEELE_ASSERT(cpuid() == 0, "uart: read() must run on the hart that services the UART");

    // The thread handle() has to wake. It is fixed for the whole wait: the UART
    // interrupt is only delivered to the boot hart's S-mode context (plic.cc),
    // which is the hart this thread runs on.
    proc::thread* const self = hart->current;

    while(true) {
        // A hart that panicked inside this driver never releases `m_lock`, so
        // report it instead of parking this one on the lock: the console is
        // already printing through the lock-free path and nothing will wake a
        // reader again.
        SEELE_ASSERT(!m_panicking.load(std::memory_order_acquire),
                     "uart: read() after the panic latch");

        std::optional<char> byte;

        // Interrupts stay masked from the empty check to the park. That is the
        // window in which the receive interrupt would find this thread still
        // running, wake nothing, and leave the byte queued until the next
        // keystroke: with SIE clear no interrupt is delivered on this hart in
        // between, so the byte either is already queued when we look or arrives
        // after the switch, with the thread already on the sleeping list. The
        // device lock is taken and released inside the window, so it keeps a
        // writer out only for the poll, not for the wait; the one path that
        // leaves it early -- the retry below -- re-enables interrupts first,
        // because nothing is registered for a wake-up yet.
        riscv::csrc(riscv::registers::sstatus, riscv::csr::sstatus::SIE);
        {
            TryLockGuard guard{m_lock};
            if(!guard.locked()) {
                // try_lock reports a lock this hart already owns as busy; a reader
                // inside the driver's own critical section can never make
                // progress, so report it instead of retrying.
                SEELE_ASSERT(!m_lock.owned_by_current_hart(), "uart: recursive read");
                // Another hart owns the lock; retry rather than register for a
                // wake-up this iteration could still miss. Nothing is published
                // yet, so interrupts can be re-enabled for the retry instead of
                // staying off for as long as the writer holds the lock.
                riscv::csrs(riscv::registers::sstatus, riscv::csr::sstatus::SIE);
                continue;
            }
            byte = take_byte_locked();
            if(!byte.has_value() && m_is_interrupt_driven) {
                SEELE_ASSERT(m_reader.load(std::memory_order_relaxed) == nullptr,
                             "uart: a second reader is waiting");
                m_reader.store(self, std::memory_order_release);
            }
        }

        if(byte.has_value()) {
            riscv::csrs(riscv::registers::sstatus, riscv::csr::sstatus::SIE);
            return *byte;
        }

        if(m_is_interrupt_driven) {
            // Park on the scheduler: the hart runs another thread (or its idle
            // loop) until this UART's receive interrupt queues a byte, wakes this
            // thread through handle(), and the trap path switches back to it.
            shed::sleep();
            m_reader.store(nullptr, std::memory_order_release);

            // sleep() switches with interrupts disabled and restores nothing;
            // the state read() was called with is re-established here (asserted
            // at the top: interrupts enabled).
            riscv::csrs(riscv::registers::sstatus, riscv::csr::sstatus::SIE);
        } else {
            // A polled UART has no interrupt that could wake a sleeping thread,
            // so wait on the hart: the timer interrupt returns here and the next
            // iteration polls the receiver again. wfi parks only a hart whose
            // interrupts are enabled.
            riscv::csrs(riscv::registers::sstatus, riscv::csr::sstatus::SIE);
            riscv::wfi();
        }
    }
}

void Ns16550::handle(uint32_t) noexcept {
    while(true) {
        if(m_panicking.load(std::memory_order_acquire)) {
            // Leave the source pending: the platform controller completes the
            // claim either way, and a hart that stopped inside this driver is
            // not going to come back to release the lock.
            return;
        }

        uint32_t received = 0;
        {
            TryLockGuard guard{m_lock};
            if(!guard.locked()) {
                // A trap cannot arrive while this hart holds `m_lock` (interrupts
                // are off in a critical section), so self-ownership here is a
                // bookkeeping bug rather than contention.
                SEELE_ASSERT(!m_lock.owned_by_current_hart(), "uart: handler owns its lock");
                continue;
            }
            if(m_is_interrupt_driven) {
                // Both conditions can be pending at once, and the receiver goes
                // first: its bytes are the ones a reader is waiting for, and
                // reading RBR is what clears the receiver interrupt.
                received = service_receive_locked();
                service_transmit_locked();
            }
        }

        // Wake the parked reader after `m_lock` is released. The switch itself
        // happens in the trap path on the way out (trap.cc), and it must not
        // start while this hart still holds the device lock. A reader that is
        // not sleeping is ignored by wake(), so a stale pointer is harmless.
        if(received != 0) {
            if(proc::thread* reader = m_reader.load(std::memory_order_acquire)) {
                shed::wake(reader);
            }
        }
        return;
    }
}

void Ns16550::set_interrupt_enable(uint8_t bits) noexcept {
    m_ier = bits;
    write(IER, bits);
}

void Ns16550::putc_polled(char c) noexcept {
    // Wait until the transmitter FIFO is empty, then hand over one byte.
    while((read(LSR) & LSR_TX_IDLE) == 0) {}
    write(THR, static_cast<uint8_t>(c));
}

void Ns16550::drain_transmit_polled() noexcept {
    while(transmit_count() != 0) {
        putc_polled(m_tx[m_tx_tail & TX_MASK]);
        ++m_tx_tail;
    }

    // The queue is empty, so let the interrupt go: an enabled THRE interrupt
    // would otherwise fire as soon as the FIFO drains, just to mask itself.
    if(m_tx_interrupt_enabled) {
        m_tx_interrupt_enabled = false;
        set_interrupt_enable(m_ier & static_cast<uint8_t>(~IER_TX_EMPTY));
    }
}

void Ns16550::start_transmit_locked() noexcept {
    if(!m_is_interrupt_driven || m_tx_interrupt_enabled || transmit_count() == 0) {
        return;
    }
    m_tx_interrupt_enabled = true;
    set_interrupt_enable(m_ier | IER_TX_EMPTY);
}

uint32_t Ns16550::service_receive_locked() noexcept {
    // One interrupt moves at most one FIFO's worth of bytes; anything beyond
    // that is still pending at the UART and is delivered by the next interrupt
    // instead of pinning the hart here.
    uint32_t received = 0;
    for(uint32_t i = 0; i < m_rx_fifo_depth; ++i) {
        if((read(LSR) & LSR_RX_READY) == 0) {
            break;
        }
        const char c = static_cast<char>(read(RBR));
        if(receive_count() >= kRxCapacity) {
            // The reader is behind. Reading the byte is still required to clear
            // the condition; dropping it is counted so a silent loss is
            // avoidable to diagnose.
            m_rx_overrun_count.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        m_rx[m_rx_head & RX_MASK] = c;
        ++m_rx_head;
        ++received;
    }
    return received;
}

void Ns16550::service_transmit_locked() noexcept {
    if(!m_tx_interrupt_enabled) {
        return;
    }

    // LSR_TX_IDLE is set when the transmit FIFO is empty, which is exactly the
    // moment the FIFO has room for its whole depth. That is why one interrupt
    // may load m_tx_fifo_depth bytes instead of one: the handler rate follows
    // the UART's FIFO, not the character time.
    if((read(LSR) & LSR_TX_IDLE) == 0) {
        return;
    }

    uint32_t room = m_tx_fifo_depth;
    while(room != 0 && transmit_count() != 0) {
        write(THR, static_cast<uint8_t>(m_tx[m_tx_tail & TX_MASK]));
        ++m_tx_tail;
        --room;
    }

    if(transmit_count() == 0) {
        m_tx_interrupt_enabled = false;
        set_interrupt_enable(m_ier & static_cast<uint8_t>(~IER_TX_EMPTY));
    }
}

uint64_t Ns16550::offset(uint32_t index) const noexcept {
    return static_cast<uint64_t>(index) << m_shift;
}

void Ns16550::write(uint32_t index, uint8_t value) const noexcept {
    if(m_width == 4) {
        m_region.write<uint32_t>(offset(index), value);
    } else {
        m_region.write<uint8_t>(offset(index), value);
    }
}

uint8_t Ns16550::read(uint32_t index) const noexcept {
    if(m_width == 4) {
        return static_cast<uint8_t>(m_region.read<uint32_t>(offset(index)));
    }
    return m_region.read<uint8_t>(offset(index));
}

device::Object* probe(device::Device& device, void* priv) noexcept {
    if(priv == nullptr) {
        return nullptr;
    }

    const device::resource* resource = device.find_resource(device::ResourceType::Mmio, 0);
    if(resource == nullptr) {
        return nullptr;
    }
    const std::optional<mmio::Region> region = mmio::map(*resource);
    if(!region.has_value()) {
        return nullptr;
    }

    // Generic 16550 bindings. Values the driver cannot honour are replaced with
    // the 8-bit, unshifted default instead of failing the probe: a UART that
    // prints is better than no console at all.
    uint32_t shift = device.u32_property("reg-shift").value_or(0);
    uint32_t width = device.u32_property("reg-io-width").value_or(1);
    const uint32_t clock = device.u32_property("clock-frequency").value_or(0);
    if(shift > 3) {
        shift = 0;
    }
    if(width != 1 && width != 4) {
        width = 1;
    }

    // The probe owns the placement new: the driver core only hands over a
    // zeroed, aligned block.
    auto* self = ::new (priv) Ns16550{*region};
    self->init(clock, shift, width);

    // The interrupt path is best effort. A device with no `interrupts`
    // property, a platform with no installed controller or a full handler
    // table all leave the driver polled, exactly as the early console runs.
    bool is_interrupt_driven = false;
    uint32_t irq = 0;
    if(const device::resource* line = device.find_resource(device::ResourceType::Irq, 0)) {
        irq = line->irq.number;
        is_interrupt_driven = trap::register_external_handler(irq, *self, IRQ_PRIORITY);
    }
    if(is_interrupt_driven) {
        self->enable_interrupts();
    }

    // The core discovers CharDevice on the returned object and attaches the
    // console; the driver does not perform subsystem attachment itself.
    if(is_interrupt_driven) {
        LOG_DEBUG("uart: {} interrupt-driven, irq {}", device.name(), irq);
    } else {
        LOG_DEBUG("uart: {} polled, no usable interrupt line", device.name());
    }

    return self;
}

}  // namespace drivers::uart
