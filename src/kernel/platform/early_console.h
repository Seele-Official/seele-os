#pragma once

// Early boot console: zero-dependency, polled-only output sink attached before
// memory management, paging, locks, interrupts, or the driver core exist.
//
// Deliberately lives in `platform/` with no dependency on `drivers/`: formal
// drivers sit above `platform/`, whereas this stage only needs synchronous
// polled output for early diagnostics and panics until a full `CharDevice`
// driver takes over via `console::attach()`.
//
// Future OpenSBI support plugs directly into this layer as a sibling
// `ConsoleSink` backend (e.g. SBI DBCN / legacy console ecall) tried ahead of
// or alongside the FDT polled UART fallback.

#include "fdt/fdt.h"

namespace platform {

// Best-effort attach an output-only early console sink.
//
// Silent contract: never logs, never asserts, never allocates, and takes no
// locks. Returning false is nonfatal -- early output is simply dropped until a
// formal console driver probes.
[[nodiscard]] bool attach_early_console(const fdt::Tree& tree) noexcept;

}  // namespace platform
