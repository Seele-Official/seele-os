#pragma once

// Stateless 16550 UART register definitions and polled I/O helper shared by
// the early boot console (platform/early_console.cc) and the formal UART
// driver (drivers/uart/ns16550.cc).
//
// Lives in `platform/` so both layers share one hardware-register definition
// while keeping `platform/` strictly below `drivers/`.

#include <cstdint>
#include <optional>

#include "mm/addr.h"
#include "platform/mmio.h"

namespace platform::ns16550 {

// Target line rate (-DSEELE_UART_BAUD).
#ifndef SEELE_UART_BAUD
#error "SEELE_UART_BAUD is not defined: build the kernel through CMake (see CMakeLists.txt)"
#endif
constexpr uint32_t UART_BAUD = SEELE_UART_BAUD;

// Register indices (converted to MMIO offsets via `index << reg_shift`).
constexpr uint32_t RBR = 0;  // receive holding register (read)
constexpr uint32_t THR = 0;  // transmit holding register (write)
constexpr uint32_t IER = 1;  // interrupt enable register
constexpr uint32_t DLL = 0;  // divisor latch low (DLAB=1)
constexpr uint32_t DLM = 1;  // divisor latch high (DLAB=1)
constexpr uint32_t IIR = 2;  // interrupt identification register (read)
constexpr uint32_t FCR = 2;  // FIFO control register (write)
constexpr uint32_t LCR = 3;  // line control register
constexpr uint32_t LSR = 5;  // line status register
constexpr uint32_t HIGHEST_REG = 7;

constexpr uint8_t FCR_FIFO_ENABLE = 1u << 0;
constexpr uint8_t FCR_FIFO_CLEAR = 3u << 1;
constexpr uint8_t LCR_EIGHT_BITS = 3u << 0;
constexpr uint8_t LCR_BAUD_LATCH = 1u << 7;
constexpr uint8_t LSR_RX_READY = 1u << 0;
constexpr uint8_t LSR_TX_IDLE = 1u << 5;
constexpr uint8_t LSR_TX_EMPTY = 1u << 6;

constexpr uint8_t IER_RX_READY = 1u << 0;
constexpr uint8_t IER_TX_EMPTY = 1u << 1;

constexpr uint8_t IIR_FIFO_MASK = 3u << 6;
constexpr uint8_t IIR_FIFO_ENABLED = 3u << 6;
constexpr uint32_t FIFO_DEPTH = 16;

// Divisor for UART_BAUD from `clock` Hz. A missing or too-low clock falls back
// to 1 (the fastest rate); an overflowing divisor clamps to 0xffff.
[[nodiscard]] constexpr uint16_t divisor_for(uint32_t clock) noexcept {
    if(clock == 0) {
        return 1;
    }
    const uint32_t divisor = clock / (16u * UART_BAUD);
    if(divisor == 0) {
        return 1;
    }
    return divisor > 0xffffu ? uint16_t{0xffff} : static_cast<uint16_t>(divisor);
}

// Validated MMIO port handle for a 16550-compatible UART.
class Port {
public:
    constexpr Port() noexcept = default;

    // Validate `region` against `shift` (0..3, fallback 0) and `width` (1 or 4,
    // fallback 1), checking base alignment and register window coverage.
    [[nodiscard]] static std::optional<Port> create(mmio::Region region,
                                                    uint32_t shift,
                                                    uint32_t width) noexcept {
        if(shift > 3) {
            shift = 0;
        }
        if(width != 1 && width != 4) {
            width = 1;
        }
        const uint64_t required = (uint64_t{HIGHEST_REG} << shift) + width;
        if(region.size() < required || (to_uint64(region.base()) & (width - 1)) != 0) {
            return std::nullopt;
        }
        return Port{region, shift, width};
    }

    // Mask interrupts, program baud divisor for `clock`, set 8N1, enable and
    // clear FIFOs.
    void configure(uint32_t clock) const noexcept {
        write_reg(IER, 0);
        write_reg(LCR, LCR_BAUD_LATCH);
        const uint16_t divisor = divisor_for(clock);
        write_reg(DLL, static_cast<uint8_t>(divisor & 0xffu));
        write_reg(DLM, static_cast<uint8_t>((divisor >> 8) & 0xffu));
        write_reg(LCR, LCR_EIGHT_BITS);
        write_reg(FCR, static_cast<uint8_t>(FCR_FIFO_ENABLE | FCR_FIFO_CLEAR));
    }

    void write_reg(uint32_t index, uint8_t value) const noexcept {
        const uint64_t offset = uint64_t{index} << m_shift;
        if(m_width == 4) {
            m_region.write<uint32_t>(offset, value);
        } else {
            m_region.write<uint8_t>(offset, value);
        }
    }

    [[nodiscard]] uint8_t read_reg(uint32_t index) const noexcept {
        const uint64_t offset = uint64_t{index} << m_shift;
        if(m_width == 4) {
            return static_cast<uint8_t>(m_region.read<uint32_t>(offset));
        }
        return m_region.read<uint8_t>(offset);
    }

    void putc_polled(char c) const noexcept {
        while((read_reg(LSR) & LSR_TX_IDLE) == 0) {}
        write_reg(THR, static_cast<uint8_t>(c));
    }

    void wait_tx_empty() const noexcept {
        while((read_reg(LSR) & LSR_TX_EMPTY) == 0) {}
    }

    void fence() const noexcept {
        m_region.fence();
    }

private:
    constexpr Port(mmio::Region region, uint32_t shift, uint32_t width) noexcept :
        m_region(region), m_shift(shift), m_width(width) {}

    mmio::Region m_region{};
    uint32_t m_shift = 0;
    uint32_t m_width = 1;
};

}  // namespace platform::ns16550
