// 16550-compatible UART driver (drivers/uart/ns16550.h).
//
// Exposes three orthogonal interfaces through `device::Implements`:
//   - `ConsoleSink`: synchronous non-scheduling kernel logging (`write_sync`,
//     `flush`) and lock-free panic output (`enter_panic`, `write_emergency`).
//   - `CharDevice`: schedulable thread-context bidirectional stream I/O
//     (`read` / `write`) backed by interrupt-driven RX/TX ring buffers and
//     `WaitQueue`s, with a sleepable writer lock (`Semaphore`) serializing
//     concurrent multi-chunk writes. Interrupt-driven mode is mandatory:
//     probe() panics if no usable IRQ line can be attached.
//   - `trap::ExternalHandler`: services RX/TX interrupts and wakes blocked
//     `CharDevice` callers outside `m_lock`.
//
// Uses the shared stateless 16550 port helper (platform/ns16550_hw.h) for
// register I/O and setup.

#include "drivers/uart/ns16550.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <span>
#include <string_view>

#include "char_device.h"
#include "console.h"
#include "console_sink.h"
#include "device_object.h"
#include "hart.h"
#include "panic.h"
#include "riscv.h"
#include "trap.h"
#include "concurrency/lock_guard.h"
#include "concurrency/semaphore.h"
#include "concurrency/spinlock.h"
#include "concurrency/wait_queue.h"
#include "drivers/registry.h"
#include "platform/device.h"
#include "platform/mmio.h"
#include "platform/ns16550_hw.h"
#include "sched/schedule.h"

namespace drivers::uart {
namespace {

using namespace platform::ns16550;

constexpr const char* MATCH[] = {"ns16550a", "ns16550", nullptr};

// Priority enabled at the interrupt controller (any non-zero value unmasks it).
constexpr uint32_t IRQ_PRIORITY = 1;

// Power-of-two ring capacities so indices wrap via bitmask.
constexpr uint32_t TX_CAPACITY = 128;
constexpr uint32_t RX_CAPACITY = 64;

class SemaphoreGuard {
public:
    explicit SemaphoreGuard(Semaphore& sem) noexcept : m_sem(sem) {
        m_sem.acquire();
    }

    ~SemaphoreGuard() noexcept {
        m_sem.release();
    }

    SemaphoreGuard(const SemaphoreGuard&) = delete;
    SemaphoreGuard& operator= (const SemaphoreGuard&) = delete;

private:
    Semaphore& m_sem;
};

class Ns16550 final : public device::Implements<ConsoleSink, CharDevice, trap::ExternalHandler> {
public:
    Ns16550(Port port, uint32_t clock) noexcept : m_port(port) {
        m_port.configure(clock);

        const uint8_t iir = m_port.read_reg(IIR);
        const bool has_fifo = (iir & IIR_FIFO_MASK) == IIR_FIFO_ENABLED;
        m_tx_fifo_depth = has_fifo ? FIFO_DEPTH : 1;
        m_rx_fifo_depth = has_fifo ? FIFO_DEPTH : 1;
    }

    Ns16550(const Ns16550&) = delete;
    Ns16550& operator= (const Ns16550&) = delete;

    // Register with the external interrupt controller and arm RX interrupts.
    // Called once from probe() before the device is published.
    [[nodiscard]] bool attach_irq(uint32_t irq) noexcept {
        if(!trap::register_external_handler(irq, *this, IRQ_PRIORITY)) {
            return false;
        }
        // Enable receiver only; TX empty is level-triggered on an idle UART and
        // is armed on demand by start_transmit_locked().
        set_interrupt_enable(IER_RX_READY);
        return true;
    }

    // ---- ConsoleSink (Path 2: synchronous log, Path 1: lock-free panic) ----

    void write_sync(std::string_view data) noexcept override {
        if(data.empty()) {
            return;
        }

        LockGuard guard{m_lock};
        // Drain any earlier async CharDevice TX before polling out kernel logs
        // so causal output order is preserved. Keep IER_TX_EMPTY armed if
        // writers are waiting so the UART IRQ wakes them outside
        // console::g_lock (avoiding any console -> cpu->lock lock edge).
        drain_transmit_polled_locked();
        for(char c: data) {
            m_port.putc_polled(c);
        }
    }

    void flush() noexcept override {
        LockGuard guard{m_lock};
        drain_transmit_polled_locked();
        // Wait for the shift register too, not just the software ring.
        m_port.wait_tx_empty();
    }

    void write_emergency(std::string_view data) noexcept override {
        for(char c: data) {
            m_port.putc_polled(c);
        }
    }

    void enter_panic() noexcept override {
        // Latch first, then fence before masking IER so a concurrent writer in
        // set_interrupt_enable() either observes the latch after its own IER
        // write and re-masks, or completes its IER write before our mask below.
        m_panicking.store(true, std::memory_order_release);
        m_port.fence();

        // Mask all UART sources without taking `m_lock` (the panicking hart may
        // hold it) so level-triggered RX/TX sources cannot storm surviving harts.
        set_interrupt_enable(0);
    }

    // ---- CharDevice (Path 3: schedulable bidirectional stream I/O) ----

    [[nodiscard]] std::size_t read(std::span<char> buffer) noexcept override {
        const cpu* hart = mycpu();
        SEELE_ASSERT(hart->current != nullptr, "uart: read() from a context without a thread");
        SEELE_ASSERT(hart->current != hart->idle, "uart: read() from the idle thread");
        SEELE_ASSERT(hart->interrupt_depth == 0, "uart: read() while holding a spinlock");
        SEELE_ASSERT(riscv::is_intr_on(), "uart: read() with interrupts disabled");

        // UART IRQ is routed to hart 0 and wake() does not send cross-hart IPIs
        // yet, so keep the reader on hart 0 for immediate interrupt wakeup.
        SEELE_ASSERT(cpuid() == 0, "uart: read() must run on the hart that services the UART");

        if(buffer.empty()) {
            return 0;
        }

        while(true) {
            WaitQueue::waiter waiter{};
            std::size_t count = 0;
            bool should_sleep = false;

            riscv::intr_off();
            {
                LockGuard guard{m_lock};
                while(count < buffer.size() && receive_count() != 0) {
                    buffer[count++] = m_rx[m_rx_tail & RX_MASK];
                    ++m_rx_tail;
                }
                if(count == 0 && !m_panicking.load(std::memory_order_acquire)) {
                    m_rx_waiters.prepare_wait(waiter);
                    should_sleep = true;
                }
            }
            if(should_sleep) {
                sched::sleep();
                riscv::intr_on();
                continue;
            }
            riscv::intr_on();
            return count;
        }
    }

    [[nodiscard]] std::size_t write(std::span<const char> data) noexcept override {
        const cpu* hart = mycpu();
        SEELE_ASSERT(hart->current != nullptr, "uart: write() from a context without a thread");
        SEELE_ASSERT(hart->current != hart->idle, "uart: write() from the idle thread");
        SEELE_ASSERT(hart->interrupt_depth == 0, "uart: write() while holding a spinlock");
        SEELE_ASSERT(riscv::is_intr_on(), "uart: write() with interrupts disabled");

        if(data.empty() || m_panicking.load(std::memory_order_acquire)) {
            return 0;
        }

        // Serialize concurrent multi-chunk writers with a sleepable lock so
        // large writes do not interleave mid-buffer while keeping `m_lock`
        // strictly short-held and non-sleeping.
        SemaphoreGuard writer_guard{m_tx_writer};

        std::size_t written = 0;
        while(written < data.size()) {
            WaitQueue::waiter waiter{};
            bool should_sleep = false;
            bool stopped = false;

            riscv::intr_off();
            {
                LockGuard guard{m_lock};
                if(m_panicking.load(std::memory_order_acquire)) {
                    stopped = true;
                } else {
                    while(written < data.size() && transmit_count() < TX_CAPACITY) {
                        m_tx[m_tx_head & TX_MASK] = data[written++];
                        ++m_tx_head;
                    }
                    start_transmit_locked();
                    if(written < data.size()) {
                        m_tx_waiters.prepare_wait(waiter);
                        should_sleep = true;
                    }
                }
            }
            if(should_sleep) {
                sched::sleep();
                riscv::intr_on();
                continue;
            }
            riscv::intr_on();
            if(stopped) {
                break;
            }
        }
        return written;
    }

    // ---- trap::ExternalHandler ----

    void handle(uint32_t) noexcept override {
        WaitQueue rx_wake{};
        WaitQueue tx_wake{};
        {
            LockGuard guard{m_lock};
            if(service_receive_locked() != 0) {
                m_rx_waiters.take_all(rx_wake);
            }
            service_transmit_locked(tx_wake);
        }

        // Wake blocked readers and writers after releasing `m_lock` so
        // `m_lock` never nests with `cpu->lock`; scheduling runs in trap.cc.
        rx_wake.wake_all();
        tx_wake.wake_all();
    }

private:
    void set_interrupt_enable(uint8_t bits) noexcept {
        // Pair enter_panic()'s latch+fence+write(IER, 0) with a full fence
        // between our IER write and the post-write latch check so RVWMO cannot
        // reorder the load ahead of the MMIO store.
        if(m_panicking.load(std::memory_order_acquire)) {
            bits = 0;
        }
        m_ier.store(bits, std::memory_order_relaxed);
        m_port.write_reg(IER, bits);
        m_port.fence();

        if(m_panicking.load(std::memory_order_acquire)) {
            m_ier.store(0, std::memory_order_relaxed);
            m_port.write_reg(IER, 0);
        }
    }

    void drain_transmit_polled_locked() noexcept {
        while(transmit_count() != 0) {
            m_port.putc_polled(m_tx[m_tx_tail & TX_MASK]);
            ++m_tx_tail;
        }

        // Only disarm IER_TX_EMPTY when no writer is sleeping on space. If a
        // writer is queued in `m_tx_waiters`, leaving IER_TX_EMPTY enabled lets
        // the next TX-empty IRQ drain `m_tx_waiters` outside `console::g_lock`.
        if(m_tx_interrupt_enabled && m_tx_waiters.empty()) {
            m_tx_interrupt_enabled = false;
            set_interrupt_enable(m_ier.load(std::memory_order_relaxed) &
                                 static_cast<uint8_t>(~IER_TX_EMPTY));
        }
    }

    void start_transmit_locked() noexcept {
        if(m_tx_interrupt_enabled || (transmit_count() == 0 && m_tx_waiters.empty())) {
            return;
        }
        m_tx_interrupt_enabled = true;
        set_interrupt_enable(m_ier.load(std::memory_order_relaxed) | IER_TX_EMPTY);
    }

    [[nodiscard]] uint32_t service_receive_locked() noexcept {
        uint32_t received = 0;
        for(uint32_t i = 0; i < m_rx_fifo_depth; ++i) {
            if((m_port.read_reg(LSR) & LSR_RX_READY) == 0) {
                break;
            }
            const char c = static_cast<char>(m_port.read_reg(RBR));
            if(receive_count() >= RX_CAPACITY) {
                m_rx_overrun_count.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            m_rx[m_rx_head & RX_MASK] = c;
            ++m_rx_head;
            ++received;
        }
        return received;
    }

    void service_transmit_locked(WaitQueue& tx_wake) noexcept {
        if(!m_tx_interrupt_enabled) {
            return;
        }
        if((m_port.read_reg(LSR) & LSR_TX_IDLE) == 0) {
            return;
        }

        uint32_t room = m_tx_fifo_depth;
        while(room != 0 && transmit_count() != 0) {
            m_port.write_reg(THR, static_cast<uint8_t>(m_tx[m_tx_tail & TX_MASK]));
            ++m_tx_tail;
            --room;
        }

        if(transmit_count() < TX_CAPACITY) {
            m_tx_waiters.take_all(tx_wake);
        }

        if(transmit_count() == 0 && m_tx_waiters.empty()) {
            m_tx_interrupt_enabled = false;
            set_interrupt_enable(m_ier.load(std::memory_order_relaxed) &
                                 static_cast<uint8_t>(~IER_TX_EMPTY));
        }
    }

    [[nodiscard]] uint32_t transmit_count() const noexcept {
        return m_tx_head - m_tx_tail;
    }

    [[nodiscard]] uint32_t receive_count() const noexcept {
        return m_rx_head - m_rx_tail;
    }

    static constexpr uint32_t TX_MASK = TX_CAPACITY - 1;
    static constexpr uint32_t RX_MASK = RX_CAPACITY - 1;
    static_assert((TX_CAPACITY & (TX_CAPACITY - 1)) == 0);
    static_assert((RX_CAPACITY & (RX_CAPACITY - 1)) == 0);

    Port m_port{};
    Spinlock m_lock{};
    bool m_tx_interrupt_enabled = false;
    std::atomic<uint8_t> m_ier{0};
    uint32_t m_tx_fifo_depth = 1;
    uint32_t m_rx_fifo_depth = 1;
    std::atomic<bool> m_panicking{false};
    Semaphore m_tx_writer{1};
    WaitQueue m_rx_waiters{};
    WaitQueue m_tx_waiters{};
    std::atomic<uint32_t> m_rx_overrun_count{0};
    char m_tx[TX_CAPACITY]{};
    uint32_t m_tx_head = 0;
    uint32_t m_tx_tail = 0;
    char m_rx[RX_CAPACITY]{};
    uint32_t m_rx_head = 0;
    uint32_t m_rx_tail = 0;
};

static_assert(alignof(Ns16550) <= driver::PRIVATE_STATE_ALIGN);

device::Object* probe(device::Device& device, void* priv) noexcept {
    if(priv == nullptr) {
        return nullptr;
    }

    const auto region = mmio::map(device);
    if(!region) {
        return nullptr;
    }

    const uint32_t clock = device.u32_property("clock-frequency").value_or(0);
    const uint32_t shift = device.u32_property("reg-shift").value_or(0);
    const uint32_t width = device.u32_property("reg-io-width").value_or(1);
    const auto port = Port::create(*region, shift, width);
    if(!port) {
        return nullptr;
    }

    auto* self = ::new (priv) Ns16550{*port, clock};

    const auto line = device.irq();
    SEELE_ASSERT(line.has_value() && self->attach_irq(line->number),
                 "uart: {} requires a usable interrupt line",
                 device.name());
    LOG_DEBUG("uart: {} interrupt-driven, irq {}", device.name(), line->number);

    return self;
}

}  // namespace

constinit const driver::descriptor NS16550_DRIVER{
    .name = "ns16550",
    .match = MATCH,
    .level = driver::InitLevel::Device,
    .priority = 0,
    .priv_size = sizeof(Ns16550),
    .probe = probe,
};

}  // namespace drivers::uart
