#pragma once

// System power-off interface and kernel shutdown entry points.
//
// Drivers that control machine power state (such as QEMU virt's `sifive_test`
// syscon finisher) implement `PowerDevice` and are attached automatically by
// the driver core when probed. Callers above the driver layer use `power::`
// entry points without depending on concrete hardware.

#include <cstdint>

class PowerDevice {
public:
    // Request system power-off with exit status `code` (0 for normal shutdown,
    // non-zero for failure exit when supported by the host/platform).
    //
    // Must be lock-free and never schedule so both normal shutdown and the
    // emergency panic path can invoke it safely. May return if hardware
    // power-off is asynchronous or unsupported.
    virtual void poweroff(uint16_t code) noexcept = 0;

protected:
    // Devices live in static storage or the driver arena and are never freed.
    ~PowerDevice() = default;
};

namespace power {

// Register `device` as the active system power controller. Called by the driver
// core when a probed object exposes `PowerDevice` via `device::dyn_cast`.
void attach(PowerDevice& device) noexcept;

// Whether a `PowerDevice` has been attached.
[[nodiscard]] bool can_poweroff() noexcept;

// Flush console output, disable local interrupts, request hardware power-off
// with `code`, and park the current hart if the machine remains running.
// Must not be called while holding console or UART spinlocks.
[[noreturn]] void poweroff(uint16_t code = 0) noexcept;

// Lock-free emergency power-off hook for panic paths. Does not flush normal
// console queues or take locks; returns if no `PowerDevice` is attached or if
// hardware shutdown does not halt execution immediately.
void poweroff_emergency(uint16_t code) noexcept;

}  // namespace power
