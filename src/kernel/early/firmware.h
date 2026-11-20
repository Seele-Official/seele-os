#pragma once
#include "fdt/fdt.h"

// The early console: the kernel's firmware phase.
//
// Everything before the driver core, the page allocator and the console lock
// goes through this one function. It behaves like firmware: it parses exactly
// one device (the console), it never logs, never asserts, never allocates, and
// its only result is a bool. A false return means the kernel keeps running with
// no output at all -- that is a supported state, not a failure to report.
//
// "SBI" here describes the shape of the stage, not a real SBI implementation:
// there is no ecall. The console is the same Ns16550 class the Device-level
// driver uses; this layer only adds the firmware-shaped rules around it.
//
// Keeping this out of driver::InitLevel is deliberate: the driver core needs
// device enumeration, a private-state arena and a console, and the console is
// exactly what is missing this early.

namespace early {

// Bring up the console and attach it as the console sink.
//
// Returns true when output is now possible, false when there is no usable
// console (or when the firmware description is malformed). No logging, no
// assertion macros, no allocation: callers must treat false as "run silently".
[[nodiscard]] bool attach_console(const fdt::Tree& tree) noexcept;

}  // namespace early
