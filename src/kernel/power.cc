#include "power.h"

#include <atomic>
#include <cstdint>

#include "console.h"
#include "riscv.h"

namespace {

std::atomic<PowerDevice*> g_device{nullptr};

[[noreturn]] void park_hart() noexcept {
    riscv::intr_off();
    riscv::csrw(riscv::registers::sie, 0);
    while(true) {
        riscv::wfi();
    }
}

}  // namespace

namespace power {

void attach(PowerDevice& device) noexcept {
    g_device.store(&device, std::memory_order_release);
}

bool can_poweroff() noexcept {
    return g_device.load(std::memory_order_acquire) != nullptr;
}

[[noreturn]] void poweroff(uint16_t code) noexcept {
    // Drain queued UART output before cutting power so the final lines reach
    // the host terminal or test harness.
    console::flush();

    riscv::intr_off();
    riscv::csrw(riscv::registers::sie, 0);

    if(PowerDevice* const device = g_device.load(std::memory_order_acquire); device != nullptr) {
        device->poweroff(code);
    }

    park_hart();
}

void poweroff_emergency(uint16_t code) noexcept {
    if(PowerDevice* const device = g_device.load(std::memory_order_acquire); device != nullptr) {
        device->poweroff(code);
    }
}

}  // namespace power
