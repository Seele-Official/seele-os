#pragma once

// Small-object kernel heap with one free list and lock per power-of-two size
// class (16, 32, 64, 128, 256, 512, 1024 bytes). Larger allocations belong to
// phymem.
//
// Each class owns one 1 GiB arena in vm::k::HEAP_BASE, grown a page at a time
// on demand and never returned to phymem. The class index is encoded in the
// address's high bits, so objects carry no header and free() needs no lookup.
//
// Requires no init() call, but is only usable after phymem::init() and
// vm::k::enable(). Also backs C++ global operator new/delete (throwing forms
// panic on OOM; nothrow forms return nullptr).

#include <cstddef>

namespace heap {

// Smallest and largest request the heap serves; larger requests panic in alloc().
constexpr std::size_t MIN_SIZE = 16;
constexpr std::size_t MAX_SIZE = 1024;

// Default alignment for plain alloc() (matches alignof(std::max_align_t) on rv64).
constexpr std::size_t DEFAULT_ALIGN = MIN_SIZE;

// Size classes: MIN_SIZE << index for index in [0, CLASS_COUNT). Must equal
// vm::k::HEAP_ARENA_COUNT.
constexpr std::size_t CLASS_COUNT = 7;

// Rounded allocation size: clamp `size` to [MIN_SIZE, MAX_SIZE] and round up to
// a power of two. Written with a loop instead of std::bit_ceil() to avoid
// libgcc's __clzdi2 on rv64 without Zbb.
[[nodiscard]] constexpr std::size_t allocation_size(std::size_t size) noexcept {
    std::size_t rounded = MIN_SIZE;
    while(rounded < size && rounded < MAX_SIZE) {
        rounded <<= 1;
    }
    return rounded;
}

// Object size of class `index` (index < CLASS_COUNT).
[[nodiscard]] constexpr std::size_t class_size(std::size_t index) noexcept {
    return MIN_SIZE << index;
}

// Class index serving `size` (saturates at CLASS_COUNT - 1; alloc() checks the
// MAX_SIZE upper bound first).
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

// Allocate at least `size` bytes aligned to `align` by promoting the size class
// to cover max(size, align). Panics if `size > MAX_SIZE`, `align > MAX_SIZE`,
// or `align` is not a power of two; returns nullptr only on OOM (phymem
// exhausted or the class's 1 GiB arena full).
[[nodiscard]] void* alloc(std::size_t size, std::size_t align) noexcept;

[[nodiscard]] inline void* alloc(std::size_t size) noexcept {
    return alloc(size, DEFAULT_ALIGN);
}

// Return a block from alloc() to its class free list; free(nullptr) is a no-op.
// Panics (SEELE_ASSERT) if `pointer` is outside the heap window, misaligned for
// its class, or in an unmapped page. Double frees are not detected.
void free(void* pointer) noexcept;

}  // namespace heap
