// Early boot console (platform/early_console.h).
//
// Self-contained polled output sink for the pre-driver boot stage. Keeps
// `platform/` strictly below `drivers/` (no circular include on
// drivers/uart/ns16550.h) and avoids instantiating interrupt/queue/lock state
// just to emit early boot logs.

#include "platform/early_console.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "char_device.h"
#include "console.h"
#include "mm/addr.h"
#include "platform/mmio.h"

namespace platform {
namespace {

#ifndef SEELE_UART_BAUD
#error "SEELE_UART_BAUD is not defined: build the kernel through CMake (see CMakeLists.txt)"
#endif
constexpr uint32_t UART_BAUD = SEELE_UART_BAUD;

// Minimal polled 16550 output sink. Future OpenSBI early logging can sit right
// beside this class as an `SbiSink final : public ConsoleSink` backend tried
// before falling back to an FDT-described UART.
class EarlyUart16550 final : public ConsoleSink {
public:
    constexpr EarlyUart16550() noexcept = default;

    [[nodiscard]] bool
        init(mmio::Region region, uint32_t clock, uint32_t shift, uint32_t width) noexcept {
        if(shift > 3) {
            shift = 0;
        }
        if(width != 1 && width != 4) {
            width = 1;
        }
        const uint64_t required = (uint64_t{HIGHEST_REG} << shift) + width;
        if(region.size() < required || (to_uint64(region.base()) & (width - 1)) != 0) {
            return false;
        }

        m_region = region;
        m_shift = shift;
        m_width = width;

        write_reg(IER, 0);
        write_reg(LCR, LCR_BAUD_LATCH);
        const uint16_t divisor = divisor_for(clock);
        write_reg(DLL, static_cast<uint8_t>(divisor & 0xffu));
        write_reg(DLM, static_cast<uint8_t>((divisor >> 8) & 0xffu));
        write_reg(LCR, LCR_EIGHT_BITS);
        write_reg(FCR, FCR_FIFO_ENABLE | FCR_FIFO_CLEAR);
        return true;
    }

    void write(const char* data, std::size_t count) noexcept override {
        write_polled(data, count);
    }

    void flush() noexcept override {
        while((read_reg(LSR) & LSR_TX_EMPTY) == 0) {}
    }

    void write_polled(const char* data, std::size_t count) noexcept override {
        if(data == nullptr) {
            return;
        }
        for(std::size_t i = 0; i < count; ++i) {
            while((read_reg(LSR) & LSR_TX_IDLE) == 0) {}
            write_reg(THR, static_cast<uint8_t>(data[i]));
        }
    }

    void flush_polled() noexcept override {}

    void enter_panic() noexcept override {}

private:
    constexpr static uint32_t THR = 0;
    constexpr static uint32_t IER = 1;
    constexpr static uint32_t DLL = 0;
    constexpr static uint32_t DLM = 1;
    constexpr static uint32_t FCR = 2;
    constexpr static uint32_t LCR = 3;
    constexpr static uint32_t LSR = 5;
    constexpr static uint32_t HIGHEST_REG = 7;

    constexpr static uint8_t FCR_FIFO_ENABLE = 1u << 0;
    constexpr static uint8_t FCR_FIFO_CLEAR = 3u << 1;
    constexpr static uint8_t LCR_EIGHT_BITS = 3u << 0;
    constexpr static uint8_t LCR_BAUD_LATCH = 1u << 7;
    constexpr static uint8_t LSR_TX_IDLE = 1u << 5;
    constexpr static uint8_t LSR_TX_EMPTY = 1u << 6;

    [[nodiscard]] constexpr static uint16_t divisor_for(uint32_t clock) noexcept {
        if(clock == 0) {
            return 1;
        }
        const uint32_t divisor = clock / (16u * UART_BAUD);
        if(divisor == 0) {
            return 1;
        }
        return divisor > 0xffffu ? uint16_t{0xffff} : static_cast<uint16_t>(divisor);
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

    mmio::Region m_region{};
    uint32_t m_shift = 0;
    uint32_t m_width = 1;
};

EarlyUart16550 g_early_uart;

// `/chosen/stdout-path` wins when it is an absolute path (stripping any
// ":options" suffix). Aliases are not resolved in this phase.
[[nodiscard]] fdt::Node console_from_chosen(const fdt::Tree& tree) noexcept {
    const fdt::Node chosen = tree.find("/chosen");
    if(!chosen) {
        return {};
    }
    const auto path = chosen.property("stdout-path").string();
    if(!path) {
        return {};
    }

    std::string_view value = *path;
    const size_t colon = value.find(':');
    if(colon != std::string_view::npos) {
        value = std::string_view{value.data(), colon};
    }
    if(value.empty() || value.front() != '/') {
        return {};
    }
    return tree.find(value);
}

[[nodiscard]] fdt::Node find_fallback_uart(const fdt::Tree& tree) noexcept {
    fdt::Node found;
    tree.walk([&](fdt::Node node) noexcept {
        if(node.enabled() && (node.is_compatible("ns16550a") || node.is_compatible("ns16550"))) {
            found = node;
            return fdt::WalkAction::Stop;
        }
        return fdt::WalkAction::Continue;
    });
    return found;
}

}  // namespace

bool attach_early_console(const fdt::Tree& tree) noexcept {
    if(!tree.valid()) {
        return false;
    }

    fdt::Node node = console_from_chosen(tree);
    if(!node) {
        node = find_fallback_uart(tree);
    }
    if(!node) {
        return false;
    }

    const auto regs = node.regs();
    const auto it = regs.begin();
    if(it == regs.end()) {
        return false;
    }

    const auto region = mmio::map(*it);
    if(!region) {
        return false;
    }

    if(!g_early_uart.init(*region,
                          node.property("clock-frequency").u32().value_or(0),
                          node.property("reg-shift").u32().value_or(0),
                          node.property("reg-io-width").u32().value_or(1))) {
        return false;
    }

    console::attach_boot(g_early_uart);
    return true;
}

}  // namespace platform
