#pragma once

// Physical page allocator.
//
// The pool is a free list of 4 KiB physical pages threaded through the pages
// themselves, so the allocator needs no storage of its own. Only the entry
// points below are exported; the free-list layout, the lock and the allocator
// object stay private to kalloc.cc.
//
// The interface speaks Paddr: alloc() returns a physical address, free() takes
// it back. Internally the free list threads
// `Node` pointers through the pages, i.e. direct-map aliases, so the pool stays
// the same when the address space moves -- only the P2V()/V2P() calls in
// kalloc.cc follow PAGE_OFFSET.
//
// init() receives the boot-time regions and keeps only RegionKind::Usable,
// subtracting every RegionKind::Reserved range. That is how the device tree
// blob, firmware reservations and the initrd stay out of the pool without
// phymem knowing anything about the FDT.

#include <span>

#include "boot/info.h"
#include "mm/addr.h"

namespace phymem {

// Build the free list from the usable physical memory regions, minus every
// reserved region.
//
// Must be called exactly once, before alloc()/free(), while only one hart is
// running. Pages that overlap the kernel image (see linker.ld: [_start, _end))
// are never added; the boot stack pool (linker.ld: .stack, after _end) is also
// skipped because harts are still standing on it, but those pages re-enter the
// pool when each hart frees its own slice after boot. A usable region that is
// not PGSIZE-aligned at one end loses the partial page at that end, because only
// whole pages can be handed out; a page that intersects a reserved region in any
// way is dropped whole.
void init(std::span<const boot::region> regions) noexcept;

// Remove the first page from the free list and return its physical address.
// The returned address is PGSIZE-aligned; dereference it through
// vm::k::P2V(). Returns null_paddr() when the pool is exhausted, so callers must
// check for it; init() never adds the zero page, so the sentinel is
// unambiguous.
[[nodiscard]] Paddr alloc() noexcept;

// Return a page obtained from alloc() to the free list. The allocator writes
// its free-list link into the page, so the caller must not use the page after
// this call.
//
// `page` must be a page obtained from alloc(): non-zero, PGSIZE-aligned, inside
// a usable region, and neither inside the kernel image nor overlapping a
// reserved range. Boot stack pages are also accepted here: each hart releases
// its slice only after switching to its idle thread's kernel stack. These
// are preconditions, not runtime errors: free() has no way to report a bad page,
// and silently dropping it would leak the page and hide the caller bug, so a
// violated precondition panics (SEELE_ASSERT, in every build). The ownership
// check matters because the free-list link is written into the page: a stray
// address would be overwritten, not rejected.
//
// Freeing the same page twice is a caller bug too, but unlike the others it is
// not detected: the second push would make the page reachable from the free
// list twice, so alloc() could hand one page to two owners. Every caller in the
// tree frees a page exactly once, and a free-list walk per free() is not worth
// paying to keep a breaking caller from being loud.
void free(Paddr page) noexcept;

}  // namespace phymem
