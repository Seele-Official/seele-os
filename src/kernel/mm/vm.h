#pragma once

#include <cstddef>
#include <optional>
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
//   - Direct map (root entries 0..127): identity-mapped image, RAM, and MMIO.
//   - Per-process user space (root entries 128..255): private U-mode mappings
//     (see namespace `vm::u`).
//   - Heap window (root entries 256..262): seven 1 GiB arenas, grown on demand.
//   - Shared kernel-stack window (root entry 508): 1 GiB arena of guarded
//     per-thread kernel stacks shared across all harts and processes.
//
// Shared kernel root entries (0..127, 256..262, and 508) freeze when init()
// returns; the system process runs directly on `pagetable()`, and additional
// process roots copy those frozen entries verbatim while keeping entries
// 128..255 private and zero-initialized.
namespace k {

// One canonical half of Sv39 (256 GiB, root entries 0..255).
constexpr uint64_t SV39_HALF_SIZE = 1ull << 38;

// Direct-map offset (currently identity mapped) and physical coverage bound
// [DIRECT_MAP_BASE, DIRECT_MAP_END) = [0, 2^37) (128 GiB, root entries 0..127).
constexpr uint64_t PAGE_OFFSET = 0;
constexpr uint64_t DIRECT_MAP_SIZE = 1ull << 37;
constexpr Paddr DIRECT_MAP_BASE = paddr(0);
constexpr Paddr DIRECT_MAP_END = paddr(DIRECT_MAP_SIZE);

[[nodiscard]] constexpr Vaddr P2V(Paddr address) noexcept {
    return vaddr(to_uint64(address) + PAGE_OFFSET);
}

// Valid only for direct-map addresses (not high-window heap or kstack Vaddrs).
[[nodiscard]] constexpr Paddr V2P(Vaddr address) noexcept {
    return paddr(to_uint64(address) - PAGE_OFFSET);
}

// Shared kernel-stack window at Sv39 root entry 508 (1 GiB, one level-2 entry).
// Slots grow downward from KSTACK_TOP: each slot has KSTACK_SIZE mapped RW/NX
// bytes above an unmapped KSTACK_GUARD_SIZE guard page, giving every thread a
// globally unique kernel stack address across all harts and processes.
constexpr Vaddr KSTACK_WINDOW_BASE = vaddr(0xffff'ffff'0000'0000ull);
constexpr uint64_t KSTACK_WINDOW_SIZE = 1ull << 30;

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
constexpr Vaddr HEAP_BASE = vaddr(~(SV39_HALF_SIZE - 1));
constexpr uint64_t HEAP_ARENA_SIZE = 1ull << 30;  // one Sv39 level-2 entry
constexpr uint64_t HEAP_ARENA_COUNT = 7;          // 16, 32, 64, 128, 256, 512, 1024

// Base address of heap arena `index` (index == HEAP_ARENA_COUNT gives the end).
[[nodiscard]] constexpr Vaddr heap_arena(uint64_t index) noexcept {
    return HEAP_BASE + index * HEAP_ARENA_SIZE;
}

// Build the shared kernel root page table once on the boot hart after
// phymem::init(), while paging is still off. Maps the kernel image (W^X), boot
// stacks, RAM, and device MMIO, then reserves empty level-1 tables for all heap
// arenas and the shared kernel-stack window so shared root entries freeze on
// return. Panics on OOM.
void init(const platform::MemoryMap& memory, std::span<const device::Device> devices);

// Physical address of the shared kernel root page table built by `init()`.
// Serves directly as the system process's page table and as the parent template
// copied by `create_process_pagetable()`.
[[nodiscard]] Paddr pagetable() noexcept;

// Allocate a root page table for an additional process after `init()`: copies
// all frozen shared kernel root entries (direct map, heap arenas, and shared
// kernel-stack window) from `pagetable()`. Returns an inactive root Paddr;
// panics on OOM.
[[nodiscard]] Paddr create_process_pagetable() noexcept;

// Allocate or recycle a guarded `KSTACK_SIZE` kernel stack slot in the shared
// root-508 window, returning its lowest mapped byte (`base`; stack top is
// `base + KSTACK_SIZE`). Requires `enable()` on the calling hart; safe to call
// concurrently across harts. Returns `std::nullopt` on OOM or slot exhaustion.
[[nodiscard]] std::optional<Vaddr> alloc_kernel_stack() noexcept;

// Return a stack `base` previously obtained from `alloc_kernel_stack()` to the
// slot free list without unmapping its pages (preserving guard pages and
// avoiding cross-hart TLB shootdowns). Panics if `base` is not a mapped slot
// base; double frees are not detected.
void free_kernel_stack(Vaddr base) noexcept;

// Encode the Sv39 `satp` CSR value for root page table `pagetable`.
[[nodiscard]] constexpr uint64_t make_satp(Paddr pagetable) noexcept {
    return riscv::csr::satp::MODE_SV39 | (to_uint64(pagetable) >> riscv::PGSHIFT);
}

// Load root `pagetable` into this hart's `satp` and execute sfence.vma.
void enable(Paddr pagetable) noexcept;

// Runtime RW/NX mapping into a pre-reserved kernel root subtree (used by
// mm/heap.cc and `alloc_kernel_stack()`), serialized by the kernel page-table
// lock with local sfence.vma. `va`, `pa`, and `size` must be page aligned, `pa`
// in the direct map, and `[va, va + size)` unmapped under reserved root
// entries. Returns false on page-table OOM (intermediate tables allocated so
// far remain linked; caller retains ownership of `pa`).
[[nodiscard]] bool mappages(Vaddr va, uint64_t size, Paddr pa) noexcept;

}  // namespace k

// Per-process user virtual address space occupying Sv39 root entries 128..255
// ([USER_BASE, USER_TOP), 128 GiB). Completely disjoint from the kernel direct
// map (0..127), heap (256..262), and shared kstack window (508), so every user
// process owns its own private subtrees under entries 128..255.
//
// User memory copy helpers translate through the process page table and copy
// via the kernel direct map rather than setting sstatus.SUM, so accidental
// direct dereference of user pointers from supervisor mode still faults.
namespace u {

constexpr Vaddr USER_BASE = vaddr(0x0000'0020'0000'0000ull);
constexpr Vaddr USER_TOP = vaddr(0x0000'0040'0000'0000ull);

// Initial user stack at the top of user space, bracketed by one unmapped page
// above (at USER_TOP - PGSIZE) and one unmapped guard page below.
constexpr uint64_t USER_STACK_SIZE = 4 * riscv::PGSIZE;
constexpr uint64_t USER_STACK_GUARD_SIZE = riscv::PGSIZE;
constexpr Vaddr USER_STACK_TOP = USER_TOP - riscv::PGSIZE;
constexpr Vaddr USER_STACK_BASE = USER_STACK_TOP - USER_STACK_SIZE;

// Exclusive upper bound for ELF PT_LOAD segments (below the stack guard page).
constexpr Vaddr USER_LOAD_LIMIT = USER_STACK_BASE - USER_STACK_GUARD_SIZE;

// True when [va, va + size) does not overflow and lies inside [USER_BASE, USER_TOP).
[[nodiscard]] constexpr bool is_user_range(Vaddr va, uint64_t size) noexcept {
    return range_fits(va, size) && va >= USER_BASE && range_end(va, size) <= USER_TOP;
}

// Map [va, va + size) -> [pa, pa + size) into a private user process root table
// with `perm | PTE_U`. Panics if `root_pa` is the kernel root, misaligned, out
// of [USER_BASE, USER_TOP), or already mapped; returns false on page-table OOM.
[[nodiscard]] bool
    map_user_pages(Paddr root_pa, Vaddr va, uint64_t size, Paddr pa, uint64_t perm) noexcept;

// Translate a user virtual byte address in `root_pa`, requiring a valid leaf
// with `PTE_U | required_perm`. Returns std::nullopt if out of range, unmapped,
// or missing any required permission bit.
[[nodiscard]] std::optional<Paddr> translate_user(Paddr root_pa,
                                                  Vaddr va,
                                                  uint64_t required_perm) noexcept;

// Verify that every page covering [va, va + size) is inside user space and
// mapped with `PTE_U | required_perm`.
[[nodiscard]] bool
    check_user_range(Paddr root_pa, Vaddr va, size_t size, uint64_t required_perm) noexcept;

// Copy `size` bytes from user virtual address `src_va` in `root_pa` into kernel
// buffer `dst`, checking `PTE_U | PTE_R` on every page crossed. Returns false
// on any invalid address or permission failure (earlier bytes may be written).
[[nodiscard]] bool copy_from_user(Paddr root_pa, void* dst, Vaddr src_va, size_t size) noexcept;

// Copy `size` bytes from kernel buffer `src` into user virtual address `dst_va`
// in `root_pa`, checking `PTE_U | PTE_R | PTE_W` on every page crossed. Returns
// false on any invalid address or permission failure (earlier bytes may be
// written; call check_user_range() first when all-or-nothing is required).
[[nodiscard]] bool copy_to_user(Paddr root_pa, Vaddr dst_va, const void* src, size_t size) noexcept;

// Free all user leaf physical pages and private intermediate page tables under
// root entries 128..255, leaving shared kernel root entries untouched. Caller
// must ensure no hart is actively translating through `root_pa`.
void destroy_user_mappings(Paddr root_pa) noexcept;

}  // namespace u

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
