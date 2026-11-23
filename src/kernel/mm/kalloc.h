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
// init() receives `platform::MemoryMap`, adding `usable()` pages while
// subtracting every `reserved()` range so the DTB, firmware reservations, and
// initrd stay out of the pool without phymem knowing anything about FDT.

#include "mm/addr.h"
#include "platform/platform.h"

namespace phymem {

// Build the free list from `memory.usable()`, minus every range in
// `memory.reserved()`, the kernel image (`[_start, _end)`), and the boot stack
// pool (`.stack`, freed later by each hart's idle thread).
//
// Must be called exactly once before alloc()/free(), while only one hart is
// running. Partial pages at unaligned region ends are dropped; any page
// overlapping a reserved range is excluded whole.
void init(const platform::MemoryMap& memory) noexcept;

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
