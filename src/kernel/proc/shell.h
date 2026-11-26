#pragma once

// Embedded user-mode shell image and process spawner.
//
// The interactive shell is built as a standalone RISC-V ELF64 executable
// (`src/user/shell/main.cc`), stripped, and embedded into the kernel via C++26
// `#embed`. `spawn_shell()` creates an isolated user process, loads the ELF64
// image with W^X mappings and a guarded user stack, and returns its initial
// U-mode thread ready to be enqueued.

#include <cstddef>
#include <span>

#include "proc/process.h"

namespace proc {

// Read-only view of the embedded stripped ELF64 shell executable.
[[nodiscard]] std::span<const std::byte> shell_image() noexcept;

// Create a fresh user process, load the embedded shell ELF into its private
// Sv39 user address space, and return its initial U-mode thread (state Created;
// caller binds and enqueues it via `sched::enqueue()`). Panics on OOM or image
// validation failure.
[[nodiscard]] thread* spawn_shell() noexcept;

}  // namespace proc
