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

// The shared kernel root page table, built once by init(). Loaded directly into
// `satp` for the system process on every hart, and used as the parent template
// when `create_process_pagetable()` clones frozen kernel subtrees into a new
// process root. Root entries reserved here are final once init() returns;
// runtime heap and kernel-stack growth only mutates lower page-table levels.
riscv::PageTable* g_kernel_pagetable = nullptr;

// Serializes runtime mutation of shared kernel page-table subtrees (heap arenas
// and the shared kernel-stack window). Every process root shares the same
// level-1 tables under those reserved root entries, so walk<true>()'s
// allocate-and-link step must not race between harts.
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

// The shared kernel-stack window occupies Sv39 root entry 508 (one 1 GiB
// level-2 entry backed by a single shared level-1 table). Every slot consists
// of `KSTACK_SIZE` mapped bytes above an unmapped `KSTACK_GUARD_SIZE` guard
// page, all contained within root entry 508.
constexpr uint64_t KSTACK_ROOT_INDEX = PX<2>(KSTACK_WINDOW_BASE);
static_assert(KSTACK_ROOT_INDEX == 508, "the kstack window must be root entry 508");
static_assert(to_uint64(KSTACK_WINDOW_BASE) % (1ull << 30) == 0,
              "the kstack window must start on an Sv39 level-2 boundary");
static_assert(KSTACK_WINDOW_SIZE == 1ull << 30,
              "the kstack window must span one 1 GiB Sv39 root entry");
static_assert(PX<2>(KSTACK_TOP - 1) == KSTACK_ROOT_INDEX,
              "the kstack window must stay inside root entry 508");
static_assert(KSTACK_SIZE % riscv::PGSIZE == 0, "a kernel stack must be page sized");
static_assert(KSTACK_GUARD_SIZE >= riscv::PGSIZE,
              "every stack needs at least one unmapped guard page below it");
static_assert(KSTACK_GUARD_SIZE % riscv::PGSIZE == 0, "a stack gap must be page sized");
static_assert(KSTACK_SLOT_SIZE % riscv::PGSIZE == 0, "a stack slot must be page sized");
static_assert(KSTACK_MAX_THREADS >= 2,
              "the kstack window must hold at least the idle and shell threads");
static_assert(KSTACK_MAX_THREADS * KSTACK_SLOT_SIZE <= KSTACK_WINDOW_SIZE,
              "thread stacks must not exceed the planned kstack window");
static_assert(PX<2>(KSTACK_BASE) == KSTACK_ROOT_INDEX,
              "slot 0's stack must start in the kstack window's root entry");
static_assert(PX<2>(kstack_slot_base(KSTACK_MAX_THREADS - 1)) == KSTACK_ROOT_INDEX,
              "the lowest stack must stay in the kstack window's root entry");
static_assert(to_uint64(kstack_slot_base(KSTACK_MAX_THREADS - 1)) - KSTACK_GUARD_SIZE >=
                  to_uint64(KSTACK_WINDOW_BASE),
              "the lowest stack's guard page must stay inside the kstack window");

// Sv39 root-table partition:
//   - 0..127:   direct map (128 GiB, frozen and copied into every process root)
//   - 128..255: private per-process user space (128 GiB, zero in kernel root)
//   - 256..262: shared kernel heap arenas (7 GiB, frozen and copied)
//   - 508:      shared kernel-stack window (1 GiB, frozen and copied)
constexpr uint64_t DIRECT_MAP_ROOT_COUNT = PX<2>(P2V(DIRECT_MAP_END));
constexpr uint64_t USER_ROOT_BEGIN = PX<2>(u::USER_BASE);
constexpr uint64_t USER_ROOT_END = PX<2>(u::USER_TOP - 1) + 1;
constexpr uint64_t HEAP_ROOT_BEGIN = PX<2>(heap_arena(0));
constexpr uint64_t HEAP_ROOT_END = PX<2>(heap_arena(HEAP_ARENA_COUNT));

static_assert(DIRECT_MAP_ROOT_COUNT == 128, "direct map must occupy root entries 0..127");
static_assert(USER_ROOT_BEGIN == DIRECT_MAP_ROOT_COUNT,
              "user space must start immediately above the direct map");
static_assert(USER_ROOT_END == 256, "user space must end at the top of Sv39's low half");
static_assert(HEAP_ROOT_BEGIN == 256, "heap arenas must start at root entry 256");
static_assert(HEAP_ROOT_END == 263, "seven heap arenas occupy root entries 256..262");
static_assert(HEAP_ROOT_END <= KSTACK_ROOT_INDEX,
              "heap root entries must stay below the kstack root entry");

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
static_assert(u::USER_BASE == P2V(DIRECT_MAP_END), "user space must begin at DIRECT_MAP_END");
static_assert(to_uint64(u::USER_STACK_TOP) % 16 == 0,
              "initial user stack pointer must preserve 16-byte psABI alignment");
static_assert(u::USER_LOAD_LIMIT > u::USER_BASE && u::USER_STACK_BASE < u::USER_STACK_TOP,
              "user load region and user stack must be non-empty");

// Intrusive free list + high-water mark for guarded kernel-stack slots.
// Freed slots thread their link into the lowest mapped bytes of the stack
// itself (identical to `phymem` and `heap`), keeping pages mapped so reuse is
// O(1) and requires no cross-hart TLB shootdown.
struct kstack_free_node {
    kstack_free_node* next;
};

Spinlock g_kstack_lock{};
kstack_free_node* g_kstack_free_list = nullptr;
uint64_t g_kstack_next_slot = 0;

// Link a fresh, zeroed level-1 table into root entry `index`, with V only so
// walk() sees a table pointer rather than a leaf. Called during init() before
// paging is enabled; the entry is frozen afterwards so process root tables can
// copy it without going stale.
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
// process root table that copied it stale.
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

void copy_frozen_root_entry(riscv::PageTable* dst, uint64_t index) noexcept {
    const riscv::PageTableEntry pte = (*g_kernel_pagetable)[index];
    if((pte & riscv::PTE_V) != 0) {
        SEELE_ASSERT((pte & PTE_RWX) == 0, "vm: kernel root entry {} is not a table link", index);
    }
    (*dst)[index] = pte;
}

}  // namespace

void init(const platform::MemoryMap& memory, std::span<const device::Device> devices) {
    // Allocate and zero the single shared kernel root page table.
    const Paddr root = phymem::alloc();
    if(is_null(root)) {
        SEELE_PANIC("vm: out of memory allocating the kernel page table");
    }
    g_kernel_pagetable = PA2PT(root);
    memset(g_kernel_pagetable, 0, riscv::PGSIZE);
    g_kstack_free_list = nullptr;
    g_kstack_next_slot = 0;

    //   .text                          R+X
    //   .rodata                        R
    //   .data + .init_array + .bss     R+W
    map_image(g_kernel_pagetable, vm::TEXT.start, vm::RODATA.start, riscv::PTE_R | riscv::PTE_X);
    map_image(g_kernel_pagetable, vm::RODATA.start, vm::DATA.start, riscv::PTE_R);
    map_image(g_kernel_pagetable, vm::DATA.start, vm::IMAGE.end, PTE_KERNEL_RW);

    // Boot stacks stay mapped in the direct map while harts switch into idle().
    map_image(g_kernel_pagetable, vm::BOOT_STACK.start, vm::BOOT_STACK.end, PTE_KERNEL_RW);

    map_physical_memory(memory);
    map_devices(devices);

    // Reserve empty level-1 tables for all seven heap arenas (256..262) and for
    // the shared kernel-stack window (508) so every shared root entry is final
    // when init() returns. Root entries 128..255 stay empty for per-process
    // user mappings.
    for(uint64_t index = USER_ROOT_BEGIN; index < USER_ROOT_END; ++index) {
        SEELE_ASSERT(((*g_kernel_pagetable)[index] & riscv::PTE_V) == 0,
                     "vm: user root entry {} is unexpectedly occupied by the direct map",
                     index);
    }
    for(uint64_t index = 0; index < HEAP_ARENA_COUNT; ++index) {
        SEELE_ASSERT(((*g_kernel_pagetable)[PX<2>(heap_arena(index))] & riscv::PTE_V) == 0,
                     "vm: the direct map already uses heap arena {}",
                     index);
    }
    SEELE_ASSERT(((*g_kernel_pagetable)[KSTACK_ROOT_INDEX] & riscv::PTE_V) == 0,
                 "vm: root entry {} is already in use",
                 KSTACK_ROOT_INDEX);

    for(uint64_t index = 0; index < HEAP_ARENA_COUNT; ++index) {
        reserve_empty_level1(g_kernel_pagetable, PX<2>(heap_arena(index)), "heap");
    }
    reserve_empty_level1(g_kernel_pagetable, KSTACK_ROOT_INDEX, "kstack");

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
        "vm: kernel page table at {:#018x}, shared kstack window [{:#018x}, {:#018x}), " "{} slots of {} bytes",
        to_uint64(V2P(vaddr(g_kernel_pagetable))),
        to_uint64(KSTACK_WINDOW_BASE),
        to_uint64(KSTACK_TOP),
        KSTACK_MAX_THREADS,
        KSTACK_SIZE);
    LOG_INFO("vm: heap window [{:#018x}, {:#018x})",
             to_uint64(HEAP_BASE),
             to_uint64(heap_arena(HEAP_ARENA_COUNT)));
}

Paddr pagetable() noexcept {
    SEELE_ASSERT(g_kernel_pagetable != nullptr, "vm: pagetable() before init()");
    return V2P(vaddr(g_kernel_pagetable));
}

Paddr create_process_pagetable() noexcept {
    SEELE_ASSERT(g_kernel_pagetable != nullptr, "vm: create_process_pagetable() before init()");

    const Paddr root_pa = phymem::alloc();
    if(is_null(root_pa)) {
        SEELE_PANIC("vm: out of memory allocating a process page table");
    }
    riscv::PageTable* root = PA2PT(root_pa);
    memset(root, 0, riscv::PGSIZE);

    for(uint64_t index = 0; index < DIRECT_MAP_ROOT_COUNT; ++index) {
        copy_frozen_root_entry(root, index);
    }
    for(uint64_t index = HEAP_ROOT_BEGIN; index < HEAP_ROOT_END; ++index) {
        copy_frozen_root_entry(root, index);
    }
    copy_frozen_root_entry(root, KSTACK_ROOT_INDEX);

    return root_pa;
}

std::optional<Vaddr> alloc_kernel_stack() noexcept {
    SEELE_ASSERT(g_kernel_pagetable != nullptr, "vm: alloc_kernel_stack() before init()");

    LockGuard guard{g_kstack_lock};
    if(g_kstack_free_list != nullptr) {
        kstack_free_node* const node = g_kstack_free_list;
        g_kstack_free_list = node->next;
        return vaddr(node);
    }

    if(g_kstack_next_slot >= KSTACK_MAX_THREADS) {
        return std::nullopt;
    }

    const uint64_t slot = g_kstack_next_slot;
    const Vaddr base = kstack_slot_base(slot);

    for(uint64_t offset = 0; offset < KSTACK_SIZE; offset += riscv::PGSIZE) {
        const Paddr page = phymem::alloc();
        if(is_null(page)) {
            if(offset != 0) {
                // Retire a partially mapped slot rather than unmapping shared
                // pages without cross-hart TLB shootdown.
                ++g_kstack_next_slot;
            }
            return std::nullopt;
        }
        if(!mappages(base + offset, riscv::PGSIZE, page)) {
            phymem::free(page);
            if(offset != 0) {
                ++g_kstack_next_slot;
            }
            return std::nullopt;
        }
    }

    ++g_kstack_next_slot;
    return base;
}

void free_kernel_stack(Vaddr base) noexcept {
    SEELE_ASSERT(g_kernel_pagetable != nullptr, "vm: free_kernel_stack() before init()");
    SEELE_ASSERT(range_contains(KSTACK_WINDOW_BASE, KSTACK_TOP, base),
                 "vm: free_kernel_stack({:#018x}) is outside the kstack window",
                 to_uint64(base));

    const uint64_t distance_from_top = KSTACK_TOP - base;
    SEELE_ASSERT(distance_from_top >= KSTACK_SIZE &&
                     (distance_from_top - KSTACK_SIZE) % KSTACK_SLOT_SIZE == 0,
                 "vm: free_kernel_stack({:#018x}) is not a slot base",
                 to_uint64(base));
    const uint64_t slot = (distance_from_top - KSTACK_SIZE) / KSTACK_SLOT_SIZE;

    LockGuard guard{g_kstack_lock};
    SEELE_ASSERT(slot < g_kstack_next_slot,
                 "vm: free_kernel_stack({:#018x}) names unmapped slot {}",
                 to_uint64(base),
                 slot);

    auto* const node = to_pointer<kstack_free_node>(base);
    node->next = g_kstack_free_list;
    g_kstack_free_list = node;
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

    // Runtime shared mappings must stay below the root entries init() reserved:
    // creating or re-pointing one here would leave every process root table
    // that copied it stale.
    require_reserved_root(va, size);

    // Other harts grow shared subtrees concurrently, and the levels below each
    // reserved entry are shared with every process root table: walk()'s
    // allocate-and-link step must stay serialized.
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

namespace u {
namespace {

constexpr uint64_t USER_ROOT_BEGIN = PX<2>(USER_BASE);
constexpr uint64_t USER_ROOT_END = PX<2>(USER_TOP - 1) + 1;
constexpr uint64_t PTE_RWX = riscv::PTE_R | riscv::PTE_W | riscv::PTE_X;

void require_process_root(Paddr root_pa, const char* caller) noexcept {
    SEELE_ASSERT(!is_null(root_pa) && is_page_aligned(root_pa) &&
                     range_contains(k::DIRECT_MAP_BASE, k::DIRECT_MAP_END, root_pa),
                 "{}: invalid root pagetable {:#x}",
                 caller,
                 to_uint64(root_pa));
    SEELE_ASSERT(root_pa != k::pagetable(),
                 "{}: must not mutate the shared kernel pagetable",
                 caller);
}

}  // namespace

bool map_user_pages(Paddr root_pa, Vaddr va, uint64_t size, Paddr pa, uint64_t perm) noexcept {
    require_process_root(root_pa, "vm::u::map_user_pages");
    SEELE_ASSERT(size != 0 && is_user_range(va, size),
                 "vm::u::map_user_pages: [{:#018x}, +{:#x}) is outside user space",
                 to_uint64(va),
                 size);
    return vm::mappages(PA2PT(root_pa), va, size, pa, perm | riscv::PTE_U);
}

std::optional<Paddr> translate_user(Paddr root_pa, Vaddr va, uint64_t required_perm) noexcept {
    if(!range_contains(USER_BASE, USER_TOP, va)) {
        return std::nullopt;
    }
    const riscv::PageTableEntry* const pte = walk<false>(PA2PT(root_pa), va);
    if(pte == nullptr) {
        return std::nullopt;
    }
    const uint64_t needed = riscv::PTE_V | riscv::PTE_U | required_perm;
    if((*pte & needed) != needed) {
        return std::nullopt;
    }
    return PTE2PA(*pte) + (to_uint64(va) & riscv::PGOFFSET_MASK);
}

bool check_user_range(Paddr root_pa, Vaddr va, size_t size, uint64_t required_perm) noexcept {
    if(!is_user_range(va, size)) {
        return false;
    }
    size_t done = 0;
    while(done < size) {
        const Vaddr cur = va + done;
        if(!translate_user(root_pa, cur, required_perm).has_value()) {
            return false;
        }
        const uint64_t page_off = to_uint64(cur) & riscv::PGOFFSET_MASK;
        const size_t step = riscv::PGSIZE - page_off;
        done += (size - done < step) ? (size - done) : step;
    }
    return true;
}

bool copy_from_user(Paddr root_pa, void* dst, Vaddr src_va, size_t size) noexcept {
    if(size == 0) {
        return true;
    }
    if(dst == nullptr || !is_user_range(src_va, size)) {
        return false;
    }

    auto* out = static_cast<std::byte*>(dst);
    size_t done = 0;
    while(done < size) {
        const Vaddr cur = src_va + done;
        const std::optional<Paddr> pa = translate_user(root_pa, cur, riscv::PTE_R);
        if(!pa.has_value()) {
            return false;
        }
        const uint64_t page_off = to_uint64(cur) & riscv::PGOFFSET_MASK;
        const size_t avail = riscv::PGSIZE - page_off;
        const size_t chunk = (size - done < avail) ? (size - done) : avail;
        memcpy(out + done, to_pointer<const void>(k::P2V(*pa)), chunk);
        done += chunk;
    }
    return true;
}

bool copy_to_user(Paddr root_pa, Vaddr dst_va, const void* src, size_t size) noexcept {
    if(size == 0) {
        return true;
    }
    if(src == nullptr || !is_user_range(dst_va, size)) {
        return false;
    }

    const auto* in = static_cast<const std::byte*>(src);
    size_t done = 0;
    while(done < size) {
        const Vaddr cur = dst_va + done;
        const std::optional<Paddr> pa = translate_user(root_pa, cur, riscv::PTE_R | riscv::PTE_W);
        if(!pa.has_value()) {
            return false;
        }
        const uint64_t page_off = to_uint64(cur) & riscv::PGOFFSET_MASK;
        const size_t avail = riscv::PGSIZE - page_off;
        const size_t chunk = (size - done < avail) ? (size - done) : avail;
        memcpy(to_pointer<void>(k::P2V(*pa)), in + done, chunk);
        done += chunk;
    }
    return true;
}

void destroy_user_mappings(Paddr root_pa) noexcept {
    require_process_root(root_pa, "vm::u::destroy_user_mappings");

    riscv::PageTable* const root = PA2PT(root_pa);
    constexpr size_t ENTRIES_PER_TABLE = riscv::PGSIZE / sizeof(riscv::PageTableEntry);

    for(uint64_t l2 = USER_ROOT_BEGIN; l2 < USER_ROOT_END; ++l2) {
        const riscv::PageTableEntry l2_pte = (*root)[l2];
        if((l2_pte & riscv::PTE_V) == 0) {
            continue;
        }
        SEELE_ASSERT((l2_pte & PTE_RWX) == 0,
                     "vm::u::destroy_user_mappings: superpage at l2 {}",
                     l2);
        const Paddr l1_pa = PTE2PA(l2_pte);
        riscv::PageTable* const l1_table = PA2PT(l1_pa);

        for(size_t l1 = 0; l1 < ENTRIES_PER_TABLE; ++l1) {
            const riscv::PageTableEntry l1_pte = (*l1_table)[l1];
            if((l1_pte & riscv::PTE_V) == 0) {
                continue;
            }
            SEELE_ASSERT((l1_pte & PTE_RWX) == 0,
                         "vm::u::destroy_user_mappings: superpage at l1 {}",
                         l1);
            const Paddr l0_pa = PTE2PA(l1_pte);
            riscv::PageTable* const l0_table = PA2PT(l0_pa);

            for(size_t l0 = 0; l0 < ENTRIES_PER_TABLE; ++l0) {
                const riscv::PageTableEntry leaf = (*l0_table)[l0];
                if((leaf & riscv::PTE_V) == 0) {
                    continue;
                }
                phymem::free(PTE2PA(leaf));
            }
            phymem::free(l0_pa);
        }
        phymem::free(l1_pa);
        (*root)[l2] = 0;
    }
}

}  // namespace u

}  // namespace vm
