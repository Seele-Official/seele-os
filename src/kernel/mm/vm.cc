#include "mm/vm.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string.h>

#include "console.h"
#include "panic.h"
#include "concurrency/lock_guard.h"
#include "concurrency/spinlock.h"
#include "mm/addr.h"
#include "mm/kalloc.h"

namespace {

// The kernel root page table, built once by init(). It is not any hart's satp:
// each process has its own root, created by create_process_pagetable() copying
// this table's low (direct map) and heap root entries. The entries here are
// final once init() returns, and runtime mapping only fills the levels below
// them, so every process root that copied an entry keeps seeing the same
// subtree.
riscv::PageTable* g_kernel_pagetable = nullptr;

// Serializes mutation of the shared heap page-table subtrees after boot. Every
// process root and the kernel root point at the same level-1 tables under the
// heap arenas, so the heap's allocate-and-link step in walk() must not race
// between harts.
Spinlock g_kernel_pt_lock{};

template <uint8_t level>
constexpr uint64_t PX(Vaddr va) noexcept {
    constexpr auto shift = riscv::PGSHIFT + (9 * (level));
    return (to_uint64(va) >> shift) & 0x1FF;  // 9 bits
}

// A page-table entry holds the physical page number in bits 53:10: the
// page-aligned physical address with the low PGSHIFT bits dropped and the
// result shifted up by PGFSHIFT, which leaves the ten flag bits free.
//
// An address at or above the end of the direct map has no PPN encoding here: it
// is a Vaddr that reached a Paddr slot, or a corrupted address, and silently
// truncating it would map a page nobody asked for.
riscv::PageTableEntry PA2PTE(Paddr pa) noexcept {
    SEELE_DASSERT(to_uint64(pa) < vm::k::DIRECT_MAP_SIZE,
                  "PA2PTE: {:#x} is not a physical address",
                  to_uint64(pa));
    return (to_uint64(pa) >> riscv::PGSHIFT) << riscv::PGFSHIFT;
}

// The inverse. Only PPN[43:0] -- PTE bits 53:10 -- name the page; bits 54-63
// are reserved (bit 63 is the N bit of Svnapot), so they are masked off instead
// of becoming part of the address. Without the mask a stray reserved bit would
// land in address bits 56-63 and turn the entry into a pointer into the heap or
// kstack window rather than into RAM.
[[nodiscard]] Paddr PTE2PA(riscv::PageTableEntry pte) noexcept {
    const uint64_t ppn = (pte >> riscv::PGFSHIFT) & riscv::PTE_PPN_MASK;
    return paddr(ppn << riscv::PGSHIFT);
}

// A table the kernel can write to is named by its direct-map alias. The PTE
// stores the physical page number, so the two directions are exactly V2P() on
// the way into a PTE and P2V() on the way out.
//
// A page table that is not inside the direct map has no alias to dereference, so
// the assert below is what keeps this the one checked crossing: without it a bad
// Paddr would become a pointer into the heap or kstack window.
[[nodiscard]] riscv::PageTable* PA2PT(Paddr pa) noexcept {
    SEELE_DASSERT(range_contains(vm::k::DIRECT_MAP_BASE, vm::k::DIRECT_MAP_END, pa),
                  "PA2PT: page table {:#x} is outside the direct map",
                  to_uint64(pa));
    return to_pointer<riscv::PageTable>(vm::k::P2V(pa));
}

[[nodiscard]] riscv::PageTableEntry PT2PTE(riscv::PageTable* table) noexcept {
    return PA2PTE(vm::k::V2P(vaddr(table)));
}

[[nodiscard]] riscv::PageTable* PTE2PT(riscv::PageTableEntry pte) noexcept {
    return PA2PT(PTE2PA(pte));
}

// The level-0 entry for `va`, or where it would be. With alloc=true, a missing
// intermediate level is allocated and linked in; the returned leaf entry is
// never filled in, so the caller decides its permission and whether finding an
// existing entry is an error.
//
// Superpages are not supported: a valid entry above level 0 must be a link to
// the next level, never a leaf. A leaf there is a superpage this kernel never
// creates, so walk() stops instead of treating its PPN as a table pointer.
template <bool alloc>
riscv::PageTableEntry* walk(riscv::PageTable* pagetable, Vaddr va) noexcept {
    template for(constexpr auto level: {2, 1}) {
        riscv::PageTableEntry* pte = &(*pagetable)[PX<level>(va)];
        if((*pte & riscv::PTE_V) != 0) {
            // A valid entry above level 0 that names a leaf is a superpage, and
            // this kernel only creates 4 KiB leaves. Treating its PPN as a table
            // pointer would dereference device memory, so stop instead.
            SEELE_ASSERT((*pte & (riscv::PTE_R | riscv::PTE_W | riscv::PTE_X)) == 0,
                         "walk: superpage at level {}, {:#x}",
                         level,
                         *pte);
            pagetable = PTE2PT(*pte);
        } else {
            if constexpr(alloc) {
                const Paddr page = phymem::alloc();
                if(is_null(page)) {
                    return nullptr;
                }
                // phymem::alloc() recycles pages, so clear the new table before
                // it joins the tree: every entry that is not written below has
                // to read as invalid, not as whatever the previous user left.
                pagetable = PA2PT(page);
                memset(pagetable, 0, riscv::PGSIZE);
                *pte = PT2PTE(pagetable) | riscv::PTE_V;
            } else {
                return nullptr;
            }
        }
    }
    return &(*pagetable)[PX<0>(va)];
}

// Fill [va, va + size) from `pa` with `perm`, allocating missing levels. With
// `skip_mapped`, an existing leaf is left alone: the direct map is described by
// independent passes (image sections, RAM, MMIO) and the image goes first.
// Without it, an existing leaf is a caller bug.
template <bool skip_mapped>
bool map_pages(riscv::PageTable* pagetable,
               Vaddr va,
               uint64_t size,
               Paddr pa,
               uint64_t perm) noexcept {
    for(uint64_t offset = 0; offset < size; offset += riscv::PGSIZE) {
        riscv::PageTableEntry* pte = walk<true>(pagetable, va + offset);
        if(pte == nullptr) {
            return false;
        }
        if constexpr(skip_mapped) {
            if((*pte & riscv::PTE_V) != 0) {
                continue;
            }
        } else {
            SEELE_ASSERT((*pte & riscv::PTE_V) == 0,
                         "mappages: {:#x} is already mapped",
                         to_uint64(va + offset));
        }
        *pte = PA2PTE(pa + offset) | perm | riscv::PTE_V;
    }
    return true;
}
}  // namespace

namespace vm {

bool mappages(riscv::PageTable* pagetable,
              Vaddr va,
              uint64_t size,
              Paddr pa,
              uint64_t perm) noexcept {
    // The hardware translates page-aligned addresses only, and every page in
    // the run gets the same permission bits, so a misaligned range is a caller
    // bug rather than something to round. Checking in every build, not just
    // debug: a misaligned `size` otherwise maps one page too many, and a
    // misaligned `va` silently rounds to the page below (PX drops the low bits).
    SEELE_ASSERT(is_page_aligned(va) && is_page_aligned(pa) && is_page_multiple(size),
                 "mappages: va={:#x} pa={:#x} size={:#x} is not page aligned",
                 to_uint64(va),
                 to_uint64(pa),
                 size);

    // A leaf has to be readable, writable or executable. At level 0 an entry
    // with V set and none of R/W/X is a reserved encoding, not an unmapped page,
    // and W without R is reserved too; both would build a table that only faults
    // at first access, far from here.
    SEELE_ASSERT((perm & (riscv::PTE_R | riscv::PTE_W | riscv::PTE_X)) != 0,
                 "mappages: permission {:#x} sets none of R/W/X",
                 perm);
    SEELE_ASSERT((perm & riscv::PTE_W) == 0 || (perm & riscv::PTE_R) != 0,
                 "mappages: permission {:#x} sets W without R",
                 perm);
    SEELE_ASSERT((perm & ~riscv::PTE_LEAF_MASK) == 0,
                 "mappages: permission {:#x} has bits outside the leaf mask",
                 perm);

    return map_pages<false>(pagetable, va, size, pa, perm);
}

namespace k {
namespace {

constexpr uint64_t PTE_KERNEL_RW = riscv::PTE_R | riscv::PTE_W;
constexpr uint64_t PTE_RWX = riscv::PTE_R | riscv::PTE_W | riscv::PTE_X;

// The per-process kernel stack window is one fixed virtual window, repeated in
// every process root table. It has to start on an Sv39 root boundary (so one
// root entry covers it) and its mapped slots have to stay inside one level-1
// entry (so create_process_pagetable() needs one private level-1 table for
// every stack of the process).
static_assert(PX<2>(KSTACK_WINDOW_BASE) == 508, "the kstack window must be root entry 508");
static_assert(to_uint64(KSTACK_WINDOW_BASE) % (1ull << 30) == 0,
              "the kstack window must start on an Sv39 level-2 boundary");
static_assert(to_uint64(KSTACK_WINDOW_BASE) % (1ull << 21) == 0,
              "the kstack window must start on an Sv39 level-1 boundary");
static_assert(KSTACK_WINDOW_SIZE == 1ull << 17, "the kstack window must stay 128 KiB");
static_assert(KSTACK_WINDOW_SIZE % riscv::PGSIZE == 0, "the kstack window must be page sized");
static_assert(KSTACK_SIZE <= KSTACK_WINDOW_SIZE, "a kernel stack must fit in its window");
static_assert(KSTACK_SIZE % riscv::PGSIZE == 0, "a kernel stack must be page sized");
static_assert(KSTACK_GUARD_SIZE >= riscv::PGSIZE,
              "every stack needs at least one unmapped guard page below it");
static_assert(KSTACK_GUARD_SIZE % riscv::PGSIZE == 0, "a stack gap must be page sized");
static_assert(KSTACK_SLOT_SIZE % riscv::PGSIZE == 0, "a stack slot must be page sized");
static_assert(KSTACK_MAX_THREADS >= 2,
              "the kstack window must hold at least the idle and shell threads");
static_assert(KSTACK_MAX_THREADS * KSTACK_SLOT_SIZE <= KSTACK_WINDOW_SIZE,
              "thread stacks must not exceed the planned kstack window");
static_assert(PX<2>(KSTACK_BASE) == PX<2>(KSTACK_WINDOW_BASE),
              "slot 0's stack must start in the kstack window's root entry");
static_assert(PX<1>(KSTACK_BASE) == PX<1>(KSTACK_TOP - 1),
              "slot 0's mapped stack must stay in one Sv39 level-1 entry");
static_assert(PX<2>(kstack_slot_base(KSTACK_MAX_THREADS - 1)) == PX<2>(KSTACK_WINDOW_BASE),
              "the lowest stack must stay in the kstack window's root entry");
static_assert(PX<1>(kstack_slot_base(KSTACK_MAX_THREADS - 1)) == PX<1>(KSTACK_TOP - 1),
              "every stack must stay in one Sv39 level-1 entry");
static_assert(to_uint64(kstack_slot_base(KSTACK_MAX_THREADS - 1)) >= to_uint64(KSTACK_WINDOW_BASE),
              "the lowest stack must stay inside the kstack window");

// The low root entries are the kernel half that a process root table copies:
// the direct map (root entries 0-255, including the image) plus the heap arenas.
// Root 508 is deliberately outside this count: it is the per-process kernel
// stack window, which every process builds privately instead of sharing.
constexpr uint64_t KERNEL_ROOT_ENTRY_COUNT = PX<2>(heap_arena(HEAP_ARENA_COUNT));
static_assert(KERNEL_ROOT_ENTRY_COUNT == 263,
              "the kernel half is the direct map plus the seven heap arenas");
static_assert(KERNEL_ROOT_ENTRY_COUNT <= PX<2>(KSTACK_WINDOW_BASE),
              "the kernel half must stay below the per-process stack root entry");

// Five assertions pin the heap window's contract: it starts at the bottom of the
// high half (which is only true while the direct map fills Sv39's low half, i.e.
// while DIRECT_MAP_SIZE is 2^38), that start is a level-2 boundary, each arena is
// a whole number of pages, the arenas are consecutive level-2 entries, and the
// window ends below the kstack window. Pinning HEAP_BASE's value is what makes a
// change to the direct map's extent fail here instead of building a window that
// overlaps it, or that is not a canonical address at all.
static_assert(to_uint64(HEAP_BASE) == 0xffff'ffc0'0000'0000ull,
              "the heap window must start at the bottom of Sv39's high half");
static_assert(to_uint64(HEAP_BASE) % HEAP_ARENA_SIZE == 0,
              "heap arena 0 must start on an Sv39 level-2 boundary");
static_assert(HEAP_ARENA_SIZE % riscv::PGSIZE == 0, "a heap arena must be a whole number of pages");
static_assert(PX<2>(heap_arena(HEAP_ARENA_COUNT - 1)) ==
                  PX<2>(heap_arena(0)) + HEAP_ARENA_COUNT - 1,
              "the heap arenas must be consecutive Sv39 level-2 entries");
static_assert(heap_arena(HEAP_ARENA_COUNT) <= KSTACK_WINDOW_BASE,
              "the heap window must stay below the kstack window");

// Link a fresh, zeroed level-1 table into root entry `index`, with V only so
// walk() sees a table pointer rather than a leaf. The caller has checked the
// entry is unused. The table is not live yet in either use -- init() builds the
// kernel root before paging is on, create_process_pagetable() builds a table
// before enable() loads it -- so clearing through the direct-map alias is
// enough; the later satp write and sfence.vma publish the tree. The entry is
// final afterwards: runtime mapping only fills levels below it, so copies of it
// in process root tables stay valid.
void reserve_empty_level1(riscv::PageTable* root, uint64_t index, const char* name) noexcept {
    const Paddr page = phymem::alloc();
    if(is_null(page)) {
        SEELE_PANIC("vm: out of memory reserving the {} root entry {}", name, index);
    }

    riscv::PageTable* table = PA2PT(page);
    memset(table, 0, riscv::PGSIZE);
    (*root)[index] = PT2PTE(table) | riscv::PTE_V;
}

// Assert every kernel root entry covering [va, va + size) was reserved by
// init(): valid and V-only, i.e. a table, not a leaf. Runtime kernel mapping
// must stay below these entries; creating or re-pointing one would leave every
// process root table that copied it stale. This checks
// the shared kernel root only -- a process's private stack subtree is built
// before the table is activated and never goes through this path.
void require_reserved_root(Vaddr va, uint64_t size) noexcept {
    SEELE_ASSERT(size != 0 && is_page_aligned(va) && is_page_multiple(size) && range_fits(va, size),
                 "vm: root range check: va={:#x} size={:#x} is not a page-aligned range",
                 to_uint64(va),
                 size);
    const Vaddr end = va + size;
    for(uint64_t index = PX<2>(va); index <= PX<2>(end - 1); ++index) {
        const riscv::PageTableEntry pte = (*g_kernel_pagetable)[index];
        SEELE_ASSERT((pte & riscv::PTE_V) != 0 && (pte & PTE_RWX) == 0,
                     "vm: root entry {} for [{:#x}, {:#x}) is not a reserved table",
                     index,
                     to_uint64(va),
                     to_uint64(end));
    }
}

// The permission bits of the leaf that maps `va` in `table`, or 0 when it is
// not mapped.
[[nodiscard]] uint64_t leaf_perm(riscv::PageTable* table, Vaddr va) noexcept {
    const riscv::PageTableEntry* pte = walk<false>(table, va);
    return pte == nullptr ? 0 : (*pte & riscv::PTE_LEAF_MASK);
}

// Round [base, base + size) out to whole pages and clamp it to the direct map.
// Returns std::nullopt when there is nothing to map. Over-covering a partial
// page is harmless: the bytes around the range may be accessed through the same
// page, and map_ranges() merges the result with its neighbours.
[[nodiscard]] std::optional<phys_range> direct_range(Paddr base, uint64_t size) noexcept {
    if(size == 0 || base >= DIRECT_MAP_END) {
        return std::nullopt;
    }
    if(size > DIRECT_MAP_END - base) {
        size = DIRECT_MAP_END - base;
    }
    // The clamped end is inside the direct map, so rounding up cannot wrap and
    // the result always covers at least one page.
    const Paddr begin = align_down(base);
    const Paddr end = align_up(range_end(base, size));
    return phys_range{begin, end - begin};
}

// Map the union of `ranges` at its direct-map alias, read/write and
// non-executable. Firmware describes the same memory more than once and in no
// particular order, so sort and merge first.
void map_ranges(std::span<phys_range> ranges) noexcept {
    const size_t merged_count = normalize_ranges(ranges);
    for(const phys_range& merged: ranges.first(merged_count)) {
        if(!map_pages<true>(g_kernel_pagetable,
                            P2V(merged.base),
                            merged.size,
                            merged.base,
                            PTE_KERNEL_RW)) {
            SEELE_PANIC("vm: out of memory building the kernel page tables");
        }
    }
}

// Map one permission group of the kernel image into `table`. linker.ld asserts
// that every group boundary is page aligned, so the range is a whole number of
// pages and no page is shared between two permission groups.
//
// The caller passes the start of the *next* group as `end`, not the group's own
// end symbol: the linker pads each group up to the next page boundary, and that
// padding belongs to the group before it. Ending at the group's own end would
// leave the padding unmapped, and the next instruction fetch or data access in
// it would fault.
void map_image(riscv::PageTable* table, std::byte* begin, std::byte* end, uint64_t perm) noexcept {
    const Vaddr va = vaddr(begin);
    if(!vm::mappages(table, va, static_cast<uint64_t>(end - begin), V2P(va), perm)) {
        SEELE_PANIC("vm: out of memory mapping the kernel image");
    }
}

// Direct map every physical range in `list` as RW/NX.
void map_phys_list(std::span<const phys_range> list) noexcept {
    phys_range ranges[platform::MAX_REGIONS];
    size_t count = 0;
    for(const phys_range& region: list) {
        if(const auto range = direct_range(region.base, region.size)) {
            SEELE_ASSERT(count < platform::MAX_REGIONS,
                         "vm: physical region count exceeds MAX_REGIONS ({})",
                         platform::MAX_REGIONS);
            ranges[count++] = *range;
        }
    }
    map_ranges(std::span{ranges, count});
}

// Direct map every usable and reserved physical range firmware described.
void map_physical_memory(const platform::MemoryMap& memory) noexcept {
    map_phys_list(memory.usable());
    map_phys_list(memory.reserved());
}

// Map every device MMIO window read/write and non-executable.
void map_devices(std::span<const device::Device> devices) noexcept {
    phys_range ranges[device::MAX_RESOURCES];
    size_t count = 0;
    for(const device::Device& entry: devices) {
        for(const phys_range& mmio: entry.mmios()) {
            if(const auto range = direct_range(mmio.base, mmio.size)) {
                SEELE_ASSERT(count < device::MAX_RESOURCES,
                             "vm: device MMIO count exceeds MAX_RESOURCES ({})",
                             device::MAX_RESOURCES);
                ranges[count++] = *range;
            }
        }
    }
    map_ranges(std::span{ranges, count});
}

}  // namespace

void init(const platform::MemoryMap& memory, std::span<const device::Device> devices) {
    // The single shared root page table every hart loads into satp. Its page
    // comes from phymem and must be cleared before walk() links the first
    // level-1 table into it.
    const Paddr root = phymem::alloc();
    if(is_null(root)) {
        SEELE_PANIC("vm: out of memory allocating the kernel page table");
    }
    g_kernel_pagetable = PA2PT(root);
    memset(g_kernel_pagetable, 0, riscv::PGSIZE);

    // Linker.ld orders the image so that each permission group starts on a page
    // boundary and the group before it is padded to that boundary. The gaps are
    // part of the preceding group (or padding), so mapping [start, next start)
    // never leaks a page across a permission change.
    //
    //   .text                          R+X
    //   .rodata                        R
    //   .data + .init_array + .bss     R+W
    map_image(g_kernel_pagetable, vm::TEXT.start, vm::RODATA.start, riscv::PTE_R | riscv::PTE_X);
    map_image(g_kernel_pagetable, vm::RODATA.start, vm::DATA.start, riscv::PTE_R);
    map_image(g_kernel_pagetable, vm::DATA.start, vm::IMAGE.end, PTE_KERNEL_RW);

    // The boot stack pool lives after _end, outside the image groups, and every
    // hart is still running on it when it enables paging with a process table
    // and, a little later, switch_thread() switches to that process's stack.
    // Firmware normally describes the pool inside a usable RAM bank, but it must
    // not depend on that: map it here, before the region pass, whose leaf-skipping
    // then leaves these pages alone. The linker script asserts the pool is page
    // aligned and a whole number of pages, which is what map_image() requires.
    map_image(g_kernel_pagetable, vm::BOOT_STACK.start, vm::BOOT_STACK.end, PTE_KERNEL_RW);

    map_physical_memory(memory);
    map_devices(devices);

    // One entry per heap arena becomes final here: link a fresh empty level-1
    // table into each. Check the whole set is still invalid first -- the direct
    // map must not have claimed an entry -- and runtime mapping then only fills
    // the levels below, so process root tables can copy the entries without the
    // copies going stale. The kernel stack window (root entry 508) is
    // deliberately not reserved here: each process builds its own private
    // level-1 table for it in create_process_pagetable().
    for(uint64_t index = 0; index < HEAP_ARENA_COUNT; ++index) {
        SEELE_ASSERT(((*g_kernel_pagetable)[PX<2>(heap_arena(index))] & riscv::PTE_V) == 0,
                     "vm: the direct map already uses heap arena {}",
                     index);
    }

    for(uint64_t index = 0; index < HEAP_ARENA_COUNT; ++index) {
        reserve_empty_level1(g_kernel_pagetable, PX<2>(heap_arena(index)), "heap");
    }

    // The point of splitting the image is W^X, and a wrong permission is only
    // discovered by the fault it causes much later. Check the first page of
    // each image group; A/D are hardware-owned and deliberately ignored here.
    SEELE_ASSERT((leaf_perm(g_kernel_pagetable, vaddr(vm::TEXT.start)) & PTE_RWX) ==
                     (riscv::PTE_R | riscv::PTE_X),
                 "vm: .text is not read/execute");
    if(vm::RODATA.size() != 0) {
        SEELE_ASSERT((leaf_perm(g_kernel_pagetable, vaddr(vm::RODATA.start)) & PTE_RWX) ==
                         riscv::PTE_R,
                     "vm: .rodata is not read-only");
    }
    SEELE_ASSERT((leaf_perm(g_kernel_pagetable, vaddr(vm::DATA.start)) & PTE_RWX) ==
                     (riscv::PTE_R | riscv::PTE_W),
                 "vm: .data is not read/write");

    LOG_INFO(
        "vm: kernel page table at {:#018x}, per-process kstack window [{:#018x}, {:#018x}), " "{} thread slots of {} bytes",
        to_uint64(V2P(vaddr(g_kernel_pagetable))),
        to_uint64(KSTACK_WINDOW_BASE),
        to_uint64(KSTACK_TOP),
        KSTACK_MAX_THREADS,
        KSTACK_SIZE);
    LOG_INFO("vm: heap window [{:#018x}, {:#018x})",
             to_uint64(HEAP_BASE),
             to_uint64(heap_arena(HEAP_ARENA_COUNT)));
}

Paddr create_process_pagetable() noexcept {
    SEELE_ASSERT(g_kernel_pagetable != nullptr, "vm: create_process_pagetable() before init()");

    // The process's own root. It must be cleared before the kernel entries are
    // copied in: every root entry the kernel half does not name has to read as
    // invalid, not as the previous user's page-table contents.
    const Paddr root_pa = phymem::alloc();
    if(is_null(root_pa)) {
        SEELE_PANIC("vm: out of memory allocating a process page table");
    }
    riscv::PageTable* root = PA2PT(root_pa);
    memset(root, 0, riscv::PGSIZE);

    // The kernel half is a copy of the frozen kernel root entries: the image,
    // direct map and heap arenas, all pointing at the same level-1 tables. That
    // is what lets the process run kernel code and use the kernel heap while
    // paying only for the root page and its private stack subtree. Every valid
    // entry must be a table link (V set, no R/W/X), exactly as init() left it.
    for(uint64_t index = 0; index < KERNEL_ROOT_ENTRY_COUNT; ++index) {
        const riscv::PageTableEntry pte = (*g_kernel_pagetable)[index];
        if((pte & riscv::PTE_V) != 0) {
            SEELE_ASSERT((pte & PTE_RWX) == 0,
                         "vm: kernel root entry {} is not a table link",
                         index);
        }
        (*root)[index] = pte;
    }

    // The per-process kernel stack root entry is private, not copied. Reserve it
    // as an empty level-1 table, so the root entry set is final before the table
    // is ever loaded. The stack leaves themselves are mapped later, one thread
    // slot at a time, by map_thread_stack(): a process gets a physical stack
    // only when a thread that needs one is created.
    constexpr uint64_t kstack_root_index = PX<2>(KSTACK_WINDOW_BASE);
    SEELE_ASSERT(((*root)[kstack_root_index] & riscv::PTE_V) == 0,
                 "vm: a copied kernel entry already uses the kstack root entry");
    reserve_empty_level1(root, kstack_root_index, "process kstack");

    return root_pa;
}

void map_thread_stack(Paddr pagetable, uint64_t index) noexcept {
    SEELE_ASSERT(g_kernel_pagetable != nullptr, "vm: map_thread_stack() before init()");
    SEELE_ASSERT(!is_null(pagetable) && is_page_aligned(pagetable) &&
                     range_contains(DIRECT_MAP_BASE, DIRECT_MAP_END, pagetable),
                 "vm: map_thread_stack: {:#x} is not a page inside the direct map",
                 to_uint64(pagetable));
    SEELE_ASSERT(index < KSTACK_MAX_THREADS,
                 "vm: thread stack slot {} exceeds the {} slots in the kstack window",
                 index,
                 KSTACK_MAX_THREADS);

    // The process root is a direct-map page, so its level-2 table is reachable
    // through the ordinary alias. Entry 508 was reserved by
    // create_process_pagetable() and must still be a V-only table link: writing
    // stack leaves below it is the only mutation allowed here.
    riscv::PageTable* root = PA2PT(pagetable);
    const riscv::PageTableEntry kstack_entry = (*root)[PX<2>(KSTACK_WINDOW_BASE)];
    SEELE_ASSERT((kstack_entry & riscv::PTE_V) != 0 &&
                     (kstack_entry & (riscv::PTE_R | riscv::PTE_W | riscv::PTE_X)) == 0,
                 "vm: process root entry {} is not the reserved kstack table",
                 PX<2>(KSTACK_WINDOW_BASE));

    // The stack subtree is private to this process. Current boot callers create
    // its kernel threads serially on one hart, so no page-table lock is needed.
    // The system-process table is already active when its threads are created;
    // flush the local TLB after installing their stack mappings.
    const Vaddr base = kstack_slot_base(index);
    for(uint64_t offset = 0; offset < KSTACK_SIZE; offset += riscv::PGSIZE) {
        const Paddr page = phymem::alloc();
        if(is_null(page)) {
            SEELE_PANIC("vm: out of memory allocating a kernel stack");
        }
        if(!vm::mappages(root, base + offset, riscv::PGSIZE, page, PTE_KERNEL_RW)) {
            SEELE_PANIC("vm: out of memory mapping a kernel stack");
        }
    }
    riscv::sfence_vma();
}

void enable(Paddr pagetable) noexcept {
    SEELE_ASSERT(g_kernel_pagetable != nullptr, "vm: enable() before init()");
    SEELE_ASSERT(!is_null(pagetable) && is_page_aligned(pagetable) &&
                     range_contains(DIRECT_MAP_BASE, DIRECT_MAP_END, pagetable),
                 "vm: enable: {:#x} is not a page inside the direct map",
                 to_uint64(pagetable));

    riscv::csrw(riscv::registers::satp, make_satp(pagetable));
    // The root and the empty level-1 tables it links were written by ordinary
    // stores; make them visible to the next translation and drop any stale
    // entries the hart may have cached while satp was 0.
    riscv::sfence_vma();
}

bool mappages(Vaddr va, uint64_t size, Paddr pa) noexcept {
    SEELE_ASSERT(g_kernel_pagetable != nullptr, "vm: mappages() before init()");

    // The heap stays below the root entries init() reserved: creating or
    // re-pointing one here would leave every process root table that copied it
    // stale.
    require_reserved_root(va, size);

    // Other harts grow other arenas concurrently, and the levels below the
    // reserved entry are shared with every process root table: the
    // allocate-and-link step of walk() must stay serialized.
    LockGuard guard{g_kernel_pt_lock};
    if(!vm::mappages(g_kernel_pagetable, va, size, pa, PTE_KERNEL_RW)) {
        return false;
    }

    // Flush this hart's TLB so it sees the mapping; the kernel has no TLB
    // shootdown.
    riscv::sfence_vma();
    return true;
}

}  // namespace k

}  // namespace vm
