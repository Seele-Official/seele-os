// Early boot console (platform/early_console.h).
//
// Self-contained polled output sink for the pre-driver boot stage. Uses the
// shared stateless 16550 register helper (platform/ns16550_hw.h) so `platform/`
// stays strictly below `drivers/` without duplicating hardware programming.

#include "platform/early_console.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "char_device.h"
#include "console.h"
#include "platform/mmio.h"
#include "platform/ns16550_hw.h"

namespace platform {
namespace {

// Minimal polled 16550 output sink. Future OpenSBI early logging can sit right
// beside this class as an `SbiSink final : public ConsoleSink` backend tried
// before falling back to an FDT-described UART.
class EarlyUart16550 final : public ConsoleSink {
public:
    constexpr EarlyUart16550() noexcept = default;

    [[nodiscard]] bool
        init(mmio::Region region, uint32_t clock, uint32_t shift, uint32_t width) noexcept {
        const auto port = ns16550::Port::create(region, shift, width);
        if(!port) {
            return false;
        }
        m_port = *port;
        m_port.configure(clock);
        return true;
    }

    void write(const char* data, std::size_t count) noexcept override {
        write_polled(data, count);
    }

    void flush() noexcept override {
        m_port.wait_tx_empty();
    }

    void write_polled(const char* data, std::size_t count) noexcept override {
        if(data == nullptr) {
            return;
        }
        for(std::size_t i = 0; i < count; ++i) {
            m_port.putc_polled(data[i]);
        }
    }

    void flush_polled() noexcept override {}

    void enter_panic() noexcept override {}

private:
    ns16550::Port m_port{};
};

EarlyUart16550 g_early_uart;

[[nodiscard]] bool is_supported_uart(fdt::Node node) noexcept {
    return node.enabled() && (node.is_compatible("ns16550a") || node.is_compatible("ns16550"));
}

// `/chosen/stdout-path` wins when it names an enabled 16550-compatible node by
// absolute path (stripping any ":options" suffix). Aliases are not resolved.
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
    const fdt::Node node = tree.find(value);
    return is_supported_uart(node) ? node : fdt::Node{};
}

[[nodiscard]] fdt::Node find_fallback_uart(const fdt::Tree& tree) noexcept {
    fdt::Node found;
    tree.walk([&](fdt::Node node) noexcept {
        if(is_supported_uart(node)) {
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
