#pragma once

// The kernel heap: a small-object allocator with one free list per size class.
//
// A request is rounded up to the next power of two, clamped at 16 and
// capped at 1024 bytes, so the heap serves exactly seven sizes: 16, 32, 64,
// 128, 256, 512 and 1024 bytes. Every object of one size comes from the same
// free list, and each free list has its own lock: allocating 16-byte and 1024-byte
// objects on two harts at the same time contends on nothing. A request above 1024
// bytes is a caller bug rather than something to round up silently -- buffers
// that large belong to phymem, which hands out whole pages.
//
// Alignment is part of the same ladder. An object of class k starts at a
// multiple of its class size from a page-aligned arena, so it is aligned to its
// class size; a caller that asks for more alignment is simply served from a
// bigger class (alloc(16, 64) takes a 64-byte object). That keeps free() free of
// headers, at the cost of some interior slack in the over-aligned case. The
// ladder ends at 1024 bytes, so that is also the largest alignment the heap can
// promise.
//
// What makes free() cheap is the address layout. Every size class owns one
// 1 GiB arena of the high window described in vm.h (HEAP_BASE, heap_arena()),
// the arenas are consecutive, and each is exactly one Sv39 level-2 entry. The
// class of a heap address is therefore in its high bits: the level-2 index of
// the address, minus the index of arena 0. A heap pointer carries its own class,
// so free() needs no per-object header, no lookup table and no search; the class
// then gives the object size, and the free list is threaded through the free
// objects themselves, the same way phymem threads its free pages.
//
// Growing is on demand: when a class has no free object, it takes one fresh page
// from phymem, maps it (via the kernel page table) at the arena's next virtual
// page, and cuts it into objects of the class. Every class size divides PGSIZE,
// so a page yields PGSIZE / size objects exactly, with no leftover tail. Nothing
// is mapped until a class is first used: an unused class costs neither physical
// memory nor page-table pages.
//
// There is no init() to call. The empty allocator is its zero state -- no free
// object, no page mapped -- and every arena base is a constant of the address
// space, so static storage alone is a ready heap. The one thing the caller
// provides is the boot order: phymem must be initialized and paging enabled,
// which is true from vm::k::enable() onwards.
//
// Pages are never returned to phymem. A heap page stays mapped and its objects
// recycle through the free list, which is what keeps the free path down to one
// lock and two stores; a heap that shrank would need to unmap pages and find the
// empty ones, which is a different tradeoff this kernel does not make.
//
// The heap is a kernel heap: it has no malloc()/free() C symbols (libc owns that
// surface) and it is not per-process. It does back the C++ operator new and
// operator delete, which heap.cc defines as thin wrappers over alloc() and
// free(): a `new` expression is therefore a plain heap allocation with MAX_SIZE
// as its size limit, and a `delete` expression is free(). The plain forms panic
// when the heap is exhausted; the nothrow forms return nullptr, as the standard
// requires. Boot first uses the heap for each hart's system process and its
// kernel threads, after enabling the shared heap mappings.

#include <cstddef>

namespace heap {

// Smallest and largest request the heap serves. A smaller request is rounded up
// to MIN_SIZE; a larger one panics in alloc() (see there).
constexpr inline std::size_t MIN_SIZE = 16;
constexpr inline std::size_t MAX_SIZE = 1024;

// The alignment a plain alloc() gives: the largest fundamental alignment, which
// is also the smallest object size. On rv64 alignof(std::max_align_t) is 16, so a
// plain allocation is already aligned for every standard type; a caller that
// needs more (a cache-line-aligned structure, say) asks for it explicitly.
constexpr inline std::size_t DEFAULT_ALIGN = MIN_SIZE;

// The size classes: MIN_SIZE << index for index in [0, CLASS_COUNT), i.e. 16,
// 32, 64, 128, 256, 512 and 1024 bytes. This must equal vm::k::HEAP_ARENA_COUNT:
// one arena per class is what makes the class readable from an address.
constexpr inline std::size_t CLASS_COUNT = 7;

// Bytes an allocation of `size` occupies: bit_ceil(size), but never below
// MIN_SIZE. The clamp comes first because bit_ceil(0) is 1, not 0.
//
// This is bit_ceil() written out rather than std::bit_ceil(), because the
// library version lowers to __clzdi2, which lives in libgcc and the kernel links
// no compiler runtime (RV64 has no clz/ctz without the bit-manipulation
// extension). Sizes above MAX_SIZE saturate at MAX_SIZE; the heap rejects them
// in alloc().
[[nodiscard]] constexpr std::size_t allocation_size(std::size_t size) noexcept {
    std::size_t rounded = MIN_SIZE;
    while(rounded < size && rounded < MAX_SIZE) {
        rounded <<= 1;
    }
    return rounded;
}

// The object size of a class: MIN_SIZE << index, with index < CLASS_COUNT.
[[nodiscard]] constexpr std::size_t class_size(std::size_t index) noexcept {
    return MIN_SIZE << index;
}

// The index of the free list that serves `size`, i.e. how many doublings
// MIN_SIZE takes to reach allocation_size(size). Counted with a loop, for the
// same libgcc reason as allocation_size(). The result is CLASS_COUNT - 1 for a
// request of MAX_SIZE or more; callers check the upper bound first (alloc()
// does).
[[nodiscard]] constexpr std::size_t class_of(std::size_t size) noexcept {
    const std::size_t rounded = allocation_size(size);
    std::size_t index = 0;
    while(class_size(index) < rounded) {
        ++index;
    }
    return index;
}

static_assert(allocation_size(0) == MIN_SIZE);
static_assert(allocation_size(1) == MIN_SIZE);
static_assert(allocation_size(16) == 16);
static_assert(allocation_size(17) == 32, "a request between two classes rounds up");
static_assert(allocation_size(255) == 256);
static_assert(allocation_size(256) == 256);
static_assert(allocation_size(257) == 512, "a request between two classes rounds up");
static_assert(allocation_size(513) == 1024);
static_assert(allocation_size(1023) == 1024);
static_assert(allocation_size(1024) == 1024);
static_assert(class_of(16) == 0);
static_assert(class_of(31) == 1);
static_assert(class_of(33) == 2);
static_assert(class_of(128) == 3);
static_assert(class_of(256) == 4);
static_assert(class_of(257) == 5);
static_assert(class_of(513) == 6);
static_assert(class_of(1024) == 6);
static_assert(class_of(MAX_SIZE) == CLASS_COUNT - 1, "the classes must end at MAX_SIZE");
static_assert(MAX_SIZE == class_size(CLASS_COUNT - 1));
static_assert(DEFAULT_ALIGN == alignof(std::max_align_t),
              "a plain allocation must be aligned for every fundamental type");

// Allocate at least `size` bytes at an address that is a multiple of `align`, or
// return nullptr when no memory is available.
//
// Alignment is met by promoting the size class, so the returned block is both
// aligned and header-free: an object of class k sits at a multiple of
// class_size(k) from a page-aligned arena, so its class size is its alignment.
// A request whose alignment is bigger than its size class is served from the
// class whose size is `align` -- alloc(16, 64) takes a 64-byte object and wastes
// 48 bytes of interior slack. `align` must be a power of two and no larger than
// MAX_SIZE, because the ladder ends there; alloc(16, 4096) has no class that
// could satisfy it and panics.
//
// `size` above MAX_SIZE panics for the same reason: the heap has no class for
// it, and rounding it up would hand the caller a block smaller than asked for.
// Both of those are request errors, not runtime ones -- nullptr is only ever
// "out of memory": phymem has no page left, or the class's 1 GiB arena is full.
[[nodiscard]] void* alloc(std::size_t size, std::size_t align) noexcept;

// Allocate at least `size` bytes with the default alignment. This is the aligned
// form above with DEFAULT_ALIGN, not a second implementation: there is one
// allocator, and "plain" is just the alignment every object already has.
[[nodiscard]] inline void* alloc(std::size_t size) noexcept {
    return alloc(size, DEFAULT_ALIGN);
}

// Return a block obtained from alloc() to the free list of its size class.
//
// The class is read from the address, so `pointer` must be exactly a block
// alloc() returned: inside the heap window, aligned to its class, in a page the
// class has already mapped. Those are preconditions, not runtime errors, and a
// violated one panics in every build (SEELE_ASSERT). free(nullptr) is a no-op,
// because a delete-expression may pass a null pointer.
//
// Double frees are not detected: a freed object is indistinguishable from a
// never-allocated one, and both are simply pushed onto the free list again.
void free(void* pointer) noexcept;

}  // namespace heap
