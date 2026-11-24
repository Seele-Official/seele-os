// The compile-time driver table.
//
// Adding a driver is one entry here plus the driver's own files; nothing in
// platform/device.h or drivers/registry.h changes. The table and every
// descriptor it references are `constinit const`, so the whole registry is
// read-only data in the image with no constructor order to worry about.
//
// This file is also the only place that names concrete drivers: the core only
// sees the table through registered().

#include <span>

#include "drivers/irq/plic.h"
#include "drivers/registry.h"
#include "drivers/uart/ns16550.h"

namespace {

constinit const driver::descriptor* const DRIVERS[] = {
    &drivers::irq::PLIC_DRIVER,
    &drivers::uart::NS16550_DRIVER,
};

}  // namespace

namespace driver {

std::span<const descriptor* const> registered() noexcept {
    return DRIVERS;
}

}  // namespace driver
