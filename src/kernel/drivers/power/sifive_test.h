#pragma once

// SiFive test finisher / syscon poweroff driver descriptor
// (drivers/power/sifive_test.cc).
//
// The device implementation is private to sifive_test.cc; the driver table only
// needs its descriptor.

#include "drivers/registry.h"

namespace drivers::power {

extern const driver::descriptor SIFIVE_TEST_DRIVER;

}  // namespace drivers::power
