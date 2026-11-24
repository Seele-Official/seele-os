#pragma once

#include <cstddef>
#include <span>

#include "riscv.h"
#include "mm/addr.h"
#include "platform/device.h"
#include "platform/platform.h"

namespace vm {

// Linker-defined kernel image boundaries (linker.ld).
//
// Each permission group starts on a 4 KiB page boundary (riscv::PGSIZE):
//   .text                         R+X
//   .rodata (incl. .srodata)      R
//   .data + .init_array + .bss    R+W
// The .stack boot-stack pool sits after _end as a separate page-aligned range.
namespace linker {

extern "C" {
    extern std::byte _start[];
    extern std::byte _end[];  // page-aligned end of the kernel image

    extern std::byte __text_start[];
    extern std::byte __text_end[];

    extern std::byte __rodata_start[];
    extern std::byte __rodata_end[];

    extern std::byte __data_start[];
    extern std::byte __data_end[];

    extern std::byte __init_array_start[];
    extern std::byte __init_array_end[];

    extern std::byte __bss_start[];
    extern std::byte __bss_end[];

    extern std::byte __boot_stack_start[];
    extern std::byte __boot_stack_end[];
}

}  // namespace linker

// Half-open [start, end) range of the kernel image. `start` is page aligned for
// group-leading sections (TEXT, RODATA, DATA); packed sections may start mid-page.
struct Section {
    std::byte* start;
    std::byte* end;

    [[nodiscard]] constexpr std::size_t size() const noexcept {
        return static_cast<std::size_t>(this->end - this->start);
    }
};

constexpr Section IMAGE{linker::_start, linker::_end};
constexpr Section TEXT{linker::__text_start, linker::__text_end};
constexpr Section RODATA{linker::__rodata_start, linker::__rodata_end};
constexpr Section DATA{linker::__data_start, linker::__data_end};
constexpr Section INIT_ARRAY{linker::__init_array_start, linker::__init_array_end};
constexpr Section BSS{linker::__bss_start, linker::__bss_end};
constexpr Section BOOT_STACK{linker::__boot_stack_start, linker::__boot_stack_end};

// Sv39 kernel address-space layout and page-table management.
//
//   - Direct map (root entries 0..255): identity-mapped image, RAM, and MMIO.
//   - Heap window (root entries 256..262): seven 1 GiB arenas, grown on demand.
//   - Per-process kstack window (root entry 508): private per-process subtree.
//
// Shared kernel root entries (0..262) freeze when init() returns; process roots
// copy them verbatim and allocate a private level-1 table for entry 508.
namespace k {

// Direct-map offset (currently identity mapped) and physical coverage bound
// [DIRECT_MAP_BASE, DIRECT_MAP_END) = [0, 2^38).
constexpr uint64_t PAGE_OFFSET = 0;
constexpr uint64_t DIRECT_MAP_SIZE = 1ull << 38;
constexpr Paddr DIRECT_MAP_BASE = paddr(0);
constexpr Paddr DIRECT_MAP_END = paddr(DIRECT_MAP_SIZE);

[[nodiscard]] constexpr Vaddr P2V(Paddr address) noexcept {
    return vaddr(to_uint64(address) + PAGE_OFFSET);
}

// Valid only for direct-map addresses (not high-window heap or kstack Vaddrs).
[[nodiscard]] constexpr Paddr V2P(Vaddr address) noexcept {
    return paddr(to_uint64(address) - PAGE_OFFSET);
}

// Per-process kernel stack window at Sv39 root entry 508 (128 KiB, one level-1
// entry). Slots grow downward from KSTACK_TOP: each slot has KSTACK_SIZE mapped
// bytes above a KSTACK_GUARD_SIZE unmapped guard page.
constexpr Vaddr KSTACK_WINDOW_BASE = vaddr(0xffff'ffff'0000'0000ull);
constexpr uint64_t KSTACK_WINDOW_SIZE = 1ull << 17;

#ifndef SEELE_KSTACK_SIZE
#error "SEELE_KSTACK_SIZE is not defined: build the kernel through CMake (see CMakeLists.txt)"
#endif
constexpr uint64_t KSTACK_SIZE = SEELE_KSTACK_SIZE;

constexpr uint64_t KSTACK_GUARD_SIZE = riscv::PGSIZE;
constexpr uint64_t KSTACK_SLOT_SIZE = KSTACK_SIZE + KSTACK_GUARD_SIZE;
constexpr uint64_t KSTACK_MAX_THREADS = KSTACK_WINDOW_SIZE / KSTACK_SLOT_SIZE;

// One past the window, and the lowest mapped address of slot 0.
constexpr Vaddr KSTACK_TOP = KSTACK_WINDOW_BASE + KSTACK_WINDOW_SIZE;
constexpr Vaddr KSTACK_BASE = KSTACK_TOP - KSTACK_SIZE;

// Top (one past highest mapped byte) and base (lowest mapped byte) of thread
// slot `index`.
[[nodiscard]] constexpr Vaddr kstack_slot_top(uint64_t index) noexcept {
    return KSTACK_TOP - index * KSTACK_SLOT_SIZE;
}

[[nodiscard]] constexpr Vaddr kstack_slot_base(uint64_t index) noexcept {
    return kstack_slot_top(index) - KSTACK_SIZE;
}

// Kernel heap window at the start of Sv39's high canonical half: one 1 GiB
// level-2 entry per heap size class (matching heap::CLASS_COUNT), mapped on
// demand so free() can derive the class directly from the address.
constexpr Vaddr HEAP_BASE = vaddr(~(DIRECT_MAP_SIZE - 1));
constexpr uint64_t HEAP_ARENA_SIZE = 1ull << 30;  // one Sv39 level-2 entry
constexpr uint64_t HEAP_ARENA_COUNT = 7;          // 16, 32, 64, 128, 256, 512, 1024

// Base address of heap arena `index` (index == HEAP_ARENA_COUNT gives the end).
[[nodiscard]] constexpr Vaddr heap_arena(uint64_t index) noexcept {
    return HEAP_BASE + index * HEAP_ARENA_SIZE;
}

// Build the shared kernel root page table once on the boot hart after
// phymem::init(), while paging is still off. Maps the kernel image (W^X), boot
// stacks, RAM, and device MMIO, then reserves empty level-1 tables for all heap
// arenas so shared root entries freeze on return. Panics on OOM.
void init(const platform::MemoryMap& memory, std::span<const device::Device> devices);

// Allocate a process root page table after init(): copies frozen kernel root
// entries (direct map + heap arenas) and reserves a private empty level-1 table
// at root entry 508 for thread stacks. Stack pages are mapped on demand by
// map_thread_stack(). Returns an inactive root Paddr; panics on OOM.
[[nodiscard]] Paddr create_process_pagetable() noexcept;

// Map thread slot `index` (< KSTACK_MAX_THREADS) as RW/NX in `pagetable` and
// flush the local TLB. May target an active table; panics on OOM or if the slot
// is already mapped.
void map_thread_stack(Paddr pagetable, uint64_t index) noexcept;

// Encode the Sv39 `satp` CSR value for root page table `pagetable`.
[[nodiscard]] constexpr uint64_t make_satp(Paddr pagetable) noexcept {
    return riscv::csr::satp::MODE_SV39 | (to_uint64(pagetable) >> riscv::PGSHIFT);
}

// Load process root `pagetable` into this hart's `satp` and execute sfence.vma.
void enable(Paddr pagetable) noexcept;

// Runtime RW/NX mapping into a pre-reserved kernel root subtree (used by
// mm/heap.cc), serialized by the kernel page-table lock with local sfence.vma.
// `va`, `pa`, and `size` must be page aligned, `pa` in the direct map, and
// `[va, va + size)` unmapped under reserved root entries. Returns false on
// page-table OOM (intermediate tables allocated so far remain linked; caller
// retains ownership of `pa`).
[[nodiscard]] bool mappages(Vaddr va, uint64_t size, Paddr pa) noexcept;

}  // namespace k

// Low-level Sv39 mapper: map [va, va + size) onto [pa, pa + size) with `perm`
// (PTE_R/W/X/U; PTE_V added here), allocating missing intermediate tables from
// phymem. Panics if misaligned, invalid `perm`, or any page is already mapped.
// Returns false on page-table OOM, leaving earlier pages mapped. Caller owns
// locking, A/D bits, and `sfence.vma`; runtime kernel mappings must go through
// vm::k::mappages() instead.
[[nodiscard]] bool mappages(riscv::PageTable* pagetable,
                            Vaddr va,
                            uint64_t size,
                            Paddr pa,
                            uint64_t perm) noexcept;

}  // namespace vm
