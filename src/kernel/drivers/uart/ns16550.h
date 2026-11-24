#pragma once

// 16550-compatible UART driver descriptor (drivers/uart/ns16550.cc).
//
// The driver implementation is private to ns16550.cc; the driver table only
// needs its descriptor.

#include "drivers/registry.h"

namespace drivers::uart {

extern const driver::descriptor NS16550_DRIVER;

}  // namespace drivers::uart
