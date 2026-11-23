#include "mm/heap.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <new>

#include "panic.h"
#include "riscv.h"
#include "concurrency/lock_guard.h"
#include "concurrency/spinlock.h"
#include "mm/addr.h"
#include "mm/kalloc.h"
#include "mm/vm.h"

namespace {

// The heap and the address layout describe the same seven classes from two
// sides: heap::class_size(index) is what one object of arena `index` occupies,
// so the two tables have to agree and the arithmetic below has to stay exact.
static_assert(heap::CLASS_COUNT == vm::k::HEAP_ARENA_COUNT,
              "the heap needs exactly one arena per size class");
static_assert(std::has_single_bit(vm::k::HEAP_ARENA_SIZE),
              "an arena must be a power of two, so the class is a slice of the address");
static_assert(std::has_single_bit(heap::MIN_SIZE) && heap::MIN_SIZE <= riscv::PGSIZE,
              "a power-of-two class size divides a page exactly");

// A free object holds the link to the next free object in its first bytes, so
// the free list needs no storage of its own -- the same trick phymem plays with
// free pages. The link lives under the heap address, which is readable and
// writable once mapped, and every object starts at a multiple of its class size
// from a page-aligned arena, so it is aligned enough for a pointer.
struct free_node {
    free_node* next;
};

static_assert(sizeof(free_node) <= heap::MIN_SIZE,
              "the smallest object must hold a free-list link");
static_assert(heap::MIN_SIZE % alignof(free_node) == 0,
              "every object must hold a link in alignment");

// One size class's mutable state. The arena itself is address space, not state,
// so it is named by vm::k::heap_arena(index) instead of being stored here.
struct freelist {
    // Unused objects of this class, or nullptr when there are none. They are
    // ordered by free time, not by address.
    free_node* free_list = nullptr;

    // How many bytes of this class's arena have been mapped, counted from the
    // arena's first page. Pages are mapped from the arena base upwards, so the
    // mapped part of the arena is exactly
    // [heap_arena(index), heap_arena(index) + mapped_bytes), and the next page to
    // map starts right at its end.
    uint64_t mapped_bytes = 0;

    // Serializes this class only. Allocating another class -- or another hart
    // allocating the same class -- is unaffected.
    Spinlock lock{};
};

// The free lists, indexed by class. "Nothing mapped, nothing free" is all zeroes,
// which is exactly what static storage starts as, so the heap needs no init call
// and no init order: the first allocation of a class maps its first page. The
// constinit spells out that this state must stay free of dynamic initialization --
// a heap allocation may happen as early as the first .init_array constructor.
constinit freelist g_lists[heap::CLASS_COUNT];

// Bytes from the start of the heap window to `address`. Only meaningful once
// in_heap_window() has accepted the address.
[[nodiscard]] constexpr uint64_t window_offset(Vaddr address) noexcept {
    return address - vm::k::HEAP_BASE;
}

[[nodiscard]] constexpr uint64_t window_size() noexcept {
    return vm::k::HEAP_ARENA_COUNT * vm::k::HEAP_ARENA_SIZE;
}

// True when `address` names a byte of the heap window. An address below
// HEAP_BASE wraps around to a huge offset, so this one comparison rejects both
// ends, and nothing outside the window (the direct map, the kernel image, a
// stack) is ever mistaken for heap.
[[nodiscard]] constexpr bool in_heap_window(Vaddr address) noexcept {
    return window_offset(address) < window_size();
}

// The class an address belongs to, read from its high bits: the window is cut
// into one arena per class, all the same size, so the offset divided by the
// arena size is the class index. The arena size is a power of two, so the
// division is a shift in the generated code -- and it is why free() needs
// neither a header nor a search.
[[nodiscard]] constexpr std::size_t class_of_address(Vaddr address) noexcept {
    return static_cast<std::size_t>(window_offset(address) / vm::k::HEAP_ARENA_SIZE);
}

// Give class `index` one more page: allocate a physical page, mmap it at the
// arena's next virtual page, and thread the page's objects onto the free list.
// Called with the class lock held, and only when that free list is empty.
//
// Returns false when there is nothing to grow into -- phymem is exhausted, or
// the arena (one level-2 entry, 1 GiB) is full -- and leaves everything
// unchanged.
[[nodiscard]] bool extend(freelist& list, std::size_t index) noexcept {
    // The arena is one level-2 entry, so running past its end means the class has
    // used up its whole 1 GiB; that is a "no memory" answer, like phymem drying
    // up, not a bug.
    if(list.mapped_bytes >= vm::k::HEAP_ARENA_SIZE) {
        return false;
    }

    const Paddr page = phymem::alloc();
    if(is_null(page)) {
        return false;
    }

    const Vaddr va = vm::k::heap_arena(index) + list.mapped_bytes;
    if(!vm::k::mappages(va, riscv::PGSIZE, page)) {
        // Nothing was mapped, so the page goes straight back. (The page-table
        // pages a half-finished walk may have allocated stay lost; that is the
        // existing mappages() boundary, and it only happens when phymem is
        // already failing.)
        phymem::free(page);
        return false;
    }
    list.mapped_bytes += riscv::PGSIZE;

    // Every class size divides PGSIZE, so the page becomes exactly
    // PGSIZE / size objects with no tail. Pushing them in increasing address
    // order leaves the page's highest address at the head of the list, so a
    // fresh page is handed out from the top down.
    const uint64_t object_size = heap::class_size(index);
    for(uint64_t offset = 0; offset < riscv::PGSIZE; offset += object_size) {
        free_node* node = to_pointer<free_node>(va + offset);
        node->next = list.free_list;
        list.free_list = node;
    }
    return true;
}

}  // namespace

namespace heap {

void* alloc(std::size_t size, std::size_t align) noexcept {
    // The heap exists for small objects. A larger request has no class, and
    // rounding it up to MAX_SIZE would hand the caller a block smaller than asked
    // for, so it is a caller bug: page-sized buffers are phymem's job.
    SEELE_ASSERT(size <= MAX_SIZE, "heap: {} bytes is above the {} byte limit", size, MAX_SIZE);

    // The class ladder ends at MAX_SIZE and every object is aligned to its class
    // size, so an alignment above MAX_SIZE has no class that could satisfy it.
    // A non-power-of-two alignment could not be satisfied exactly either.
    SEELE_ASSERT(align != 0 && (align & (align - 1)) == 0,
                 "heap: alignment {} is not a power of two",
                 align);
    SEELE_ASSERT(align <= MAX_SIZE,
                 "heap: alignment {} is above the {} byte limit",
                 align,
                 MAX_SIZE);

    // Promote the class until its object size covers both the request and the
    // alignment: an object of class k starts at a multiple of class_size(k) from
    // a page-aligned arena, so the class size is also the alignment it gets.
    const std::size_t index = class_of(size > align ? size : align);
    freelist& list = g_lists[index];

    LockGuard guard{list.lock};
    if(list.free_list == nullptr && !extend(list, index)) {
        return nullptr;
    }
    free_node* node = list.free_list;
    list.free_list = node->next;
    return node;
}

void free(void* pointer) noexcept {
    // A delete-expression may pass a null pointer, and operator delete has to
    // accept one, so the no-op belongs here rather than at every call site.
    if(pointer == nullptr) {
        return;
    }

    // free() cannot report a bad pointer, and pushing one onto a free list would
    // overwrite whatever it points at -- the same reasoning phymem::free() uses.
    // Every check below therefore stops the kernel in every build.
    const Vaddr address = vaddr(pointer);
    SEELE_ASSERT(in_heap_window(address),
                 "heap: free({:#018x}) is outside the heap window",
                 to_uint64(address));

    // The class, and with it the object size and the free list, comes from the
    // address alone. This is the whole point of the window layout.
    const std::size_t index = class_of_address(address);
    const uint64_t object_size = class_size(index);
    freelist& list = g_lists[index];

    LockGuard guard{list.lock};

    // An aligned address inside a mapped page always names an object slot, and
    // the mapped part of the arena is exactly [heap_arena(index), +mapped_bytes):
    // both checks together reject a pointer into an unmapped page, an interior
    // pointer, and a pointer to another class. mapped_bytes is read under the
    // lock because that is what extend() writes it under.
    const uint64_t arena_offset = address - vm::k::heap_arena(index);
    SEELE_ASSERT(arena_offset % object_size == 0,
                 "heap: free({:#018x}) is not a {}-byte object",
                 to_uint64(address),
                 object_size);
    SEELE_ASSERT(arena_offset < list.mapped_bytes,
                 "heap: free({:#018x}) is in a page the heap never mapped",
                 to_uint64(address));

    free_node* node = reinterpret_cast<free_node*>(pointer);
    node->next = list.free_list;
    list.free_list = node;
}

}  // namespace heap

// ---------------------------------------------------------------------------
// C++ operator new / operator delete
// ---------------------------------------------------------------------------
//
// The replaceable global allocation functions are the C++ face of the heap above:
// every form forwards to heap::alloc() or heap::free(), so `new` and `delete`
// cost one call and share the same size classes as the rest of the kernel. The
// heap's contract shows through in three places:
//
//  - A size or an alignment above MAX_SIZE has no class, so heap::alloc() stops
//    the kernel on it. That includes the nothrow forms: nothrow means "exhausted
//    memory returns nullptr", not "an impossible request is forgiven".
//  - The throwing forms panic when the heap is exhausted instead of throwing
//    std::bad_alloc. Exceptions are off (-fno-exceptions), and nothing above
//    operator new could handle an allocation failure anyway.
//  - The nothrow forms return nullptr when the heap is exhausted, which is what
//    the standard and libstdc++'s internal uses expect from them.
//
// Every delete form is one call to heap::free(): the size class lives in the
// address (see the header), so the size and the alignment a delete-expression
// may pass are not needed, and a mismatch between a sized delete and the
// allocation would go undetected. free(nullptr) is a no-op, which operator
// delete has to accept.

namespace {

// The throwing forms' common path. heap::alloc() already aligns by promoting the
// size class, so handing back its block is all that is left; what separates this
// from the nothrow forms is only what a nullptr means.
[[nodiscard]] void* cxx_alloc(std::size_t size, std::size_t align) noexcept {
    void* object = heap::alloc(size, align);
    if(object == nullptr) {
        SEELE_PANIC("new: out of memory for a {}-byte object aligned to {}", size, align);
    }
    return object;
}

}  // namespace

void* operator new (std::size_t size) {
    return cxx_alloc(size, heap::DEFAULT_ALIGN);
}

void* operator new[] (std::size_t size) {
    return cxx_alloc(size, heap::DEFAULT_ALIGN);
}

void* operator new (std::size_t size, const std::nothrow_t&) noexcept {
    return heap::alloc(size, heap::DEFAULT_ALIGN);
}

void* operator new[] (std::size_t size, const std::nothrow_t&) noexcept {
    return heap::alloc(size, heap::DEFAULT_ALIGN);
}

void* operator new (std::size_t size, std::align_val_t align) {
    return cxx_alloc(size, static_cast<std::size_t>(align));
}

void* operator new[] (std::size_t size, std::align_val_t align) {
    return cxx_alloc(size, static_cast<std::size_t>(align));
}

void* operator new (std::size_t size, std::align_val_t align, const std::nothrow_t&) noexcept {
    return heap::alloc(size, static_cast<std::size_t>(align));
}

void* operator new[] (std::size_t size, std::align_val_t align, const std::nothrow_t&) noexcept {
    return heap::alloc(size, static_cast<std::size_t>(align));
}

void operator delete (void* object) noexcept {
    heap::free(object);
}

void operator delete[] (void* object) noexcept {
    heap::free(object);
}

void operator delete (void* object, std::size_t) noexcept {
    heap::free(object);
}

void operator delete[] (void* object, std::size_t) noexcept {
    heap::free(object);
}

void operator delete (void* object, const std::nothrow_t&) noexcept {
    heap::free(object);
}

void operator delete[] (void* object, const std::nothrow_t&) noexcept {
    heap::free(object);
}

void operator delete (void* object, std::align_val_t) noexcept {
    heap::free(object);
}

void operator delete[] (void* object, std::align_val_t) noexcept {
    heap::free(object);
}

void operator delete (void* object, std::size_t, std::align_val_t) noexcept {
    heap::free(object);
}

void operator delete[] (void* object, std::size_t, std::align_val_t) noexcept {
    heap::free(object);
}

void operator delete (void* object, std::align_val_t, const std::nothrow_t&) noexcept {
    heap::free(object);
}

void operator delete[] (void* object, std::align_val_t, const std::nothrow_t&) noexcept {
    heap::free(object);
}
