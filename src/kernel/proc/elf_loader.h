#pragma once

// Kernel ELF64 user-image loader.
//
// Validates an ELF64 RISC-V executable, maps its PT_LOAD segments into a
// private process page table under `vm::u` (preserving W^X and zeroing .bss),
// maps the initial guarded user stack, and issues `fence.i`.

#include <cstddef>
#include <optional>
#include <span>

#include "mm/addr.h"

namespace elf {

struct loaded_user_image {
    Vaddr entry{};
    Vaddr user_sp{};
};

// Load `image` and an initial user stack into `user_pt` (from
// `vm::k::create_process_pagetable()`). On failure, frees all user mappings
// allocated in `user_pt` and returns `std::nullopt`.
[[nodiscard]] std::optional<loaded_user_image>
    load_user_elf(Paddr user_pt, std::span<const std::byte> image) noexcept;

}  // namespace elf
