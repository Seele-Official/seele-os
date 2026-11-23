#pragma once

#include <cstddef>
#include <span>

#include "riscv.h"
#include "mm/addr.h"
#include "platform/device.h"
#include "platform/platform.h"

namespace vm {

// Kernel image layout, as defined by linker.ld.
//
// Sections are grouped by the permissions they need, and only the first section
// of a group starts on a 4 KiB page boundary -- the page size the kernel uses
// for paging, matching riscv::PGSIZE in riscv.h -- so each group is covered by
// whole pages:
//
//   .text       R+X  code
//   .rodata     R    read-only data, including .srodata
//   .data       R+W  initialized writable data, including .sdata
//   .init_array R+W  pointers to the static constructors
//   .bss        R+W  zero-initialized data, including .sbss
//
// .data, .init_array and .bss share R+W, so they are packed together and mapped
// as one group. The .stack pool (the per-hart boot stacks) lives after _end, so
// it is a separate range outside the image; see BOOT_STACK below.
//
// Sections that share the permissions of the one before them are packed with
// their natural alignment only, so switching permissions never needs a
// page-aligned boundary inside a group.
//
// The symbols themselves are emitted by the linker script, so they are only
// declared here and must never be defined in C++.
namespace linker {

extern "C" {
    // First byte of the kernel image (_entry) and the first byte past it. _end
    // is rounded up to a page, so pages that overlap [_start, _end) must never
    // enter the free pool.
    extern std::byte _start[];
    extern std::byte _end[];

    // .text
    extern std::byte __text_start[];
    extern std::byte __text_end[];

    // .rodata
    extern std::byte __rodata_start[];
    extern std::byte __rodata_end[];

    // .data, including .sdata
    extern std::byte __data_start[];
    extern std::byte __data_end[];

    // .init_array: an array of void (*)() static constructors.
    extern std::byte __init_array_start[];
    extern std::byte __init_array_end[];

    // .bss, including .sbss
    extern std::byte __bss_start[];
    extern std::byte __bss_end[];

    // .stack: the per-hart boot stacks. The pool is one page-aligned run after
    // _end; see boot/stack.h for the per-hart slice offsets.
    extern std::byte __boot_stack_start[];
    extern std::byte __boot_stack_end[];
}

}  // namespace linker

// A half-open [start, end) range of the kernel image. `start` is page aligned
// for every section that begins a permission group (TEXT, RODATA, DATA); a
// section packed behind another one of the same group may start mid-page.
struct Section {
    std::byte* start;
    std::byte* end;

    [[nodiscard]] constexpr std::size_t size() const noexcept {
        return static_cast<std::size_t>(this->end - this->start);
    }
};

// The kernel image proper: code, data and bss. _end is rounded up to a page, so
// the range is a whole number of pages. The .stack pool is after _end and is
// not part of the image.
constexpr inline Section IMAGE{linker::_start, linker::_end};
constexpr inline Section TEXT{linker::__text_start, linker::__text_end};
constexpr inline Section RODATA{linker::__rodata_start, linker::__rodata_end};
constexpr inline Section DATA{linker::__data_start, linker::__data_end};
constexpr inline Section INIT_ARRAY{linker::__init_array_start, linker::__init_array_end};
constexpr inline Section BSS{linker::__bss_start, linker::__bss_end};
constexpr inline Section BOOT_STACK{linker::__boot_stack_start, linker::__boot_stack_end};

// The kernel address space: a direct map of physical memory, the heap window and
// the per-process kernel stack window. The kernel image and RAM/MMIO are mapped
// once by vm::k::init() into the shared kernel root page table, whose root
// entries are final when init() returns. Every process then gets its own root
// page table (vm::k::create_process_pagetable()): the kernel's low and heap root
// entries are copied verbatim, so all processes share the same subtrees, while
// the per-process kernel stack slots occupy a private subtree at the same high
// virtual address in each table. That shared/private split is what lets a
// process keep its own stacks without duplicating the direct map.
//
// The heap window is a third, independent high region; see HEAP_BASE below.
//
// Every physical page the kernel may touch is visible at PAGE_OFFSET + pa,
// which is what makes a Paddr usable at all -- the kernel cannot dereference
// one directly. These two functions are the only place that arithmetic
// happens, so switching PAGE_OFFSET moves the whole direct map; the new range
// must stay disjoint from the high stack window below.
//
// PAGE_OFFSET is zero: the kernel image, RAM and MMIO are identity mapped, and
// only the per-process kernel stacks get a second, high mapping. The image stays
// at its link-time addresses, so its sections can be mapped with different
// permissions without relocating the code that is already running.
//
// They live here, not in mm/addr.h, because they are address-space policy:
// Paddr and Vaddr only describe a value; a conversion belongs next to the
// layout that makes it true, not in the type header.
namespace k {

// Offset between a physical address and its direct-map alias. Identity mapped:
// PAGE_OFFSET is zero.
constexpr inline uint64_t PAGE_OFFSET = 0;

// How much physical memory the direct map covers: [0, DIRECT_MAP_SIZE). It is a
// design bound, not a hardware one -- it keeps the direct map inside Sv39's low
// half, and it is what mmio::map() and phymem::init() both check against.
constexpr inline uint64_t DIRECT_MAP_SIZE = 1ull << 38;

// The direct map as a half-open range of physical addresses. A Paddr in
// [DIRECT_MAP_BASE, DIRECT_MAP_END) has a kernel alias and nothing else does, so
// a caller testing an address -- rather than clamping one to the map's end --
// uses range_contains() with these bounds instead of comparing against a length.
constexpr inline Paddr DIRECT_MAP_BASE = paddr(0);
constexpr inline Paddr DIRECT_MAP_END = paddr(DIRECT_MAP_SIZE);

[[nodiscard]] constexpr Vaddr P2V(Paddr address) noexcept {
    return vaddr(to_uint64(address) + PAGE_OFFSET);
}

// Only valid for addresses inside the direct map: a Vaddr that is not a
// P2V() result has no physical address to recover. A kernel stack is also
// mapped at a high virtual window, and that window is not a direct-map address,
// so a stack is addressed through its Vaddr and never converted back to a
// Paddr. Its physical pages come from phymem, so they still have their ordinary
// direct-map alias like every pool page; what is not recorded is their Paddr.
// The kernel uses the stack through the high window, not through that alias.
[[nodiscard]] constexpr Paddr V2P(Vaddr address) noexcept {
    return paddr(to_uint64(address) - PAGE_OFFSET);
}

// The per-process kernel stack window: the one part of the kernel address space
// that is not a direct map.
//
// Every process owns the same KSTACK_WINDOW_BASE..KSTACK_TOP virtual window,
// but each has its own page table and maps its own physical pages there. Two
// processes therefore never share a stack even though their stack addresses are
// identical.
//
// The window is divided into per-thread slots. Slot 0 is at the top of the
// window and slot i sits KSTACK_SLOT_SIZE bytes lower. A slot is KSTACK_SIZE
// bytes of mapped stack followed by KSTACK_GUARD_SIZE unmapped bytes below it,
// so a stack that overflows runs into its own guard page instead of silently
// into the next thread's stack. proc::make_thread() hands out the next slot
// from the per-process counter and records the slot in the thread; the counter
// is bounded by KSTACK_MAX_THREADS, the number of slots that fit in the window,
// so one process can never map past its planned stack space.
//
// KSTACK_WINDOW_BASE sits in Sv39 root entry 508, near the top of the high
// canonical half and above the direct-map range `[0, DIRECT_MAP_SIZE)`. A
// 128 KiB window is one level-1 entry, so a process needs one private level-1
// table and a few level-0 tables for all of its stacks, regardless of hart
// count.
constexpr inline Vaddr KSTACK_WINDOW_BASE = vaddr(0xffff'ffff'0000'0000ull);
constexpr inline uint64_t KSTACK_WINDOW_SIZE = 1ull << 17;

// Bytes of kernel stack in each thread slot.
//
// A build parameter (-DSEELE_KSTACK_SIZE): the boot stacks in boot/stack.h and
// the per-process kernel stacks are separate budgets, because the boot stacks
// hold the whole boot path (FDT parsing, driver probing, the page-table build)
// while these hold the call paths from a thread's first post-boot trap onwards.
// A port that runs the deeper of the two larger raises that one, with the
// static_asserts in vm.cc keeping the value inside its window.
#ifndef SEELE_KSTACK_SIZE
#error "SEELE_KSTACK_SIZE is not defined: build the kernel through CMake (see CMakeLists.txt)"
#endif
constexpr inline uint64_t KSTACK_SIZE = SEELE_KSTACK_SIZE;

// The unmapped gap between two neighbouring thread stacks: one page below the
// mapped part of each slot. KSTACK_MAX_THREADS is derived from the window, so
// a build that makes the stack too large to fit at least two threads fails its
// static_assert instead of silently exceeding the planned space.
constexpr inline uint64_t KSTACK_GUARD_SIZE = riscv::PGSIZE;
constexpr inline uint64_t KSTACK_SLOT_SIZE = KSTACK_SIZE + KSTACK_GUARD_SIZE;
constexpr inline uint64_t KSTACK_MAX_THREADS = KSTACK_WINDOW_SIZE / KSTACK_SLOT_SIZE;

// The address one past the window, and the lowest address of slot 0's stack.
// The slot helpers below name the same pair for any other thread.
constexpr inline Vaddr KSTACK_TOP = KSTACK_WINDOW_BASE + KSTACK_WINDOW_SIZE;
constexpr inline Vaddr KSTACK_BASE = KSTACK_TOP - KSTACK_SIZE;

// The top (one past the highest mapped byte) and base (lowest mapped byte) of
// thread slot `index`. The stack grows down from the top; the slot's guard is
// the KSTACK_GUARD_SIZE bytes directly below its base.
[[nodiscard]] constexpr Vaddr kstack_slot_top(uint64_t index) noexcept {
    return KSTACK_TOP - index * KSTACK_SLOT_SIZE;
}

[[nodiscard]] constexpr Vaddr kstack_slot_base(uint64_t index) noexcept {
    return kstack_slot_top(index) - KSTACK_SIZE;
}

// The kernel heap window: one arena per heap size class, at the bottom of
// Sv39's high canonical half.
//
// The low half of Sv39 is the direct map, and it fills that half exactly: the
// highest address with bit 38 clear is 2^38 - 1, so HEAP_BASE -- the first
// address with bit 38 set, and therefore the first canonical high-half address
// -- is 2^64 - DIRECT_MAP_SIZE. Level-2 entry 256 + k of the kernel page table
// is arena k, which is the whole trick behind heap::free(): the high bits of a
// heap pointer, i.e. its level-2 index, name its size class, so a class needs no
// per-object header.
//
// Each arena is one level-2 entry (1 GiB) with its own subtree of page tables,
// and pages are mapped into it on demand (see mm/heap.h), so the window is
// virtual space only -- an unused class costs nothing but its entry. Seven GiB is
// more virtual space than any machine this kernel targets has RAM, and the
// window ends far below the kstack window.
//
// HEAP_ARENA_COUNT is the heap's class count (mm/heap.h: heap::CLASS_COUNT); the
// heap asserts the two agree, and vm::k::init() asserts that the direct map did
// not claim these entries.
constexpr inline Vaddr HEAP_BASE = vaddr(~(DIRECT_MAP_SIZE - 1));
constexpr inline uint64_t HEAP_ARENA_SIZE = 1ull << 30;  // one Sv39 level-2 entry
constexpr inline uint64_t HEAP_ARENA_COUNT = 7;          // 16, 32, 64, 128, 256, 512, 1024

// First address of heap arena `index`. Valid for HEAP_ARENA_COUNT too, where it
// names the end of the window.
[[nodiscard]] constexpr Vaddr heap_arena(uint64_t index) noexcept {
    return HEAP_BASE + index * HEAP_ARENA_SIZE;
}

// Build the single shared kernel page table.
//
// Must be called once by the boot hart after phymem::init(), while paging is
// still off. It allocates a root page from phymem and maps the kernel image
// section by section with the permissions from linker.ld, the boot stack pool,
// the rest of RAM and every MMIO resource as read/write non-executable. It then
// reserves each heap arena's root entry: those entries are checked to be invalid
// (the direct map did not claim them), and each is linked to a fresh, zeroed
// level-1 page table with the valid bit and no R/W/X. They are final when init()
// returns; no later mapping touches a root entry. init() does not create or map
// any process page table. Every process, starting with each hart's idle
// process, gets one from create_process_pagetable() and loads it in enable().
//
// A page-table allocation failure panics: there is no meaningful degraded boot.
//
// `devices` is the enumerated platform bus (platform/device.h). It is passed
// instead of having vm include the bus, so the address space stays below the
// driver model in the include graph.
void init(const platform::MemoryMap& memory, std::span<const device::Device> devices);

// Build a fresh process root page table and reserve its kernel stack subtree.
//
// Called once per process, after vm::k::init() and before that process runs,
// while the kernel root is frozen. Steps:
//
//  1. allocate a root page from phymem and zero it;
//  2. copy root entries [0, KERNEL_ROOT_ENTRY_COUNT) -- the direct map with the
//     kernel image plus the heap arenas -- verbatim from the kernel root. The
//     copies point at the same level-1 tables, so the new table sees the same
//     image, RAM, MMIO and heap without duplicating any of them;
//  3. reserve this process's kernel stack root entry (PX<2>(KSTACK_WINDOW_BASE),
//     i.e. 508) as a fresh, zeroed level-1 table -- private to this process, not
//     copied from the kernel root.
//
// No physical stack page is allocated or mapped here: each thread gets its own
// slot when proc::make_thread() maps it with map_thread_stack(). Keeping the
// physical stack pages out of the page-table builder is what makes a per-thread
// stack possible without a second, process-wide mapping.
//
// The returned root is not active yet: the caller loads it with enable().
// An allocation failure panics; a process with no page table cannot run. The
// root entry set is final when this function returns (only leaf mappings below
// entry 508 are added later), which is the same freeze rule the kernel root
// follows.
[[nodiscard]] Paddr create_process_pagetable() noexcept;

// Map thread slot `index`'s kernel stack into `pagetable`, read/write and
// non-executable.
//
// Called by proc::make_thread() once per thread, after the process root
// exists and its stack root entry was reserved by create_process_pagetable().
// The caller is also responsible for the bound: make_thread() refuses a slot at
// or past KSTACK_MAX_THREADS, which is the number of slots the window holds. The
// mapping may target an active table: boot enables the system process's table
// before creating its kernel threads. Flush the local TLB before returning.
//
// A physical-page or page-table allocation failure panics: a thread whose
// stack could not be mapped must not start. Repeated mapping of the same slot
// trips the mappages() "already mapped" assertion.
void map_thread_stack(Paddr pagetable, uint64_t index) noexcept;

// Load `pagetable`, a root page table built by create_process_pagetable(), into
// this hart's satp and flush its TLB.
//
// Boot calls this on each hart with that hart's system-process root: the boot
// hart after vm::k::init(), secondaries after the shared mappings are published.
// The shared kernel root itself is not activated; each system-process root
// combines the shared kernel mappings with its private thread-stack subtree.
void enable(Paddr pagetable) noexcept;

// Map [va, va + size) onto the physical run starting at `pa` in the shared
// kernel page table, read/write and non-executable, then flush this hart's TLB.
//
// This is the grow path of the kernel heap (mm/heap.cc), which maps one fresh
// phymem page at a time into a heap arena. Both ends must be page aligned and
// `pa` must be inside the direct map, i.e. what phymem::alloc() returns. The
// target range must not be mapped yet, and every root entry covering it must
// have been reserved by vm::k::init() as a level-1 table: this function asserts
// that the entry is valid and a table pointer (V set, R/W/X clear) before
// mapping below it, never creating or re-pointing a root entry. The root
// entries are final after init(); a process root table copies them, so the
// copies stay valid as the heap grows below them.
//
// Returns false when the page-table pages needed to reach `va` cannot be
// allocated; nothing is mapped in that case, but the levels walk() already
// allocated are left in place (see vm::mappages()). The caller owns `pa` either way.
[[nodiscard]] bool mappages(Vaddr va, uint64_t size, Paddr pa) noexcept;

}  // namespace k

// Map `size` bytes at `va` onto the physical run starting at `pa`. `va` and `pa`
// must be page aligned and `size` a whole number of pages; `pa` must lie inside
// the direct map, because every mapped page has to be reachable by the kernel.
// `va` may be a direct-map address (the identity mappings), a heap arena
// address (k::mappages()), or the per-process kernel stack window
// (create_process_pagetable()).
//
// `perm` holds the RISC-V permission bits (PTE_R / PTE_W / PTE_X / PTE_U); the
// valid bit and the physical page number are added here. Missing intermediate
// page-table levels are allocated from phymem::alloc() and cleared before use.
//
// k::init() uses it to build the kernel table, and create_process_pagetable()
// uses it to map a process's stack into the process root it just built. Only
// k::init() and that per-process builder may create the level-1 tables below
// their root: the runtime kernel entry point (k::mappages()) asserts its heap
// root entries were already reserved by init(), so a runtime call only allocates
// level-0 tables and leaves. After init(), a direct call on the kernel root is
// unguarded: runtime kernel mappings must use that checked entry point instead.
// Three things are the caller's business: the A/D bits, `sfence.vma` after
// changing a live table, and tearing the table down when a run fails half way --
// a failed call leaves the pages it already mapped in place.
[[nodiscard]] bool mappages(riscv::PageTable* pagetable,
                            Vaddr va,
                            uint64_t size,
                            Paddr pa,
                            uint64_t perm) noexcept;

}  // namespace vm
