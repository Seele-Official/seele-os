#pragma once

// SiFive PLIC driver descriptor (drivers/irq/plic.cc).
//
// The controller implementation is private to plic.cc; the driver table only
// needs its descriptor.

#include "drivers/registry.h"

namespace drivers::irq {

extern const driver::descriptor PLIC_DRIVER;

}  // namespace drivers::irq
