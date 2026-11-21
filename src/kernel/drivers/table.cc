// The compile-time driver table.
//
// Adding a driver is one line here plus the driver's own files; nothing in
// platform/device.h or drivers/registry.h changes. The table is
// `constinit const`, so the whole registry is read-only data in the image with
// no constructor order to worry about.
//
// This file is also the only place that names concrete drivers: the core only
// sees the table through registered().

#include <span>

#include "drivers/irq/plic.h"
#include "drivers/registry.h"
#include "drivers/uart/ns16550.h"

namespace {

constinit const driver::descriptor kDrivers[] = {
    // Core level: every Device-level driver may register an interrupt source,
    // so the controller has to be bound (and installed in trap.h) first.
    SEELE_DRIVER(plic,
                 drivers::irq::kMatch,
                 driver::InitLevel::Core,
                 0,
                 drivers::irq::kPrivSize,
                 drivers::irq::probe),
    // The core discovers the returned object's character-device interface and
    // installs it as the console.
    SEELE_DRIVER(ns16550,
                 drivers::uart::kMatch,
                 driver::InitLevel::Device,
                 0,
                 drivers::uart::kPrivSize,
                 drivers::uart::probe),
};

}  // namespace

namespace driver {

std::span<const descriptor> registered() noexcept {
    return kDrivers;
}

}  // namespace driver
