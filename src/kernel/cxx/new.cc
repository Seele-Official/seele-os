// Global replaceable C++ operator new / operator delete backed by the kernel
// small-object heap (mm/heap.h).
//
// Every form forwards to heap::alloc() or heap::free(), so `new` and `delete`
// cost one call and share the same size classes as the rest of the kernel. The
// heap's contract shows through in three places:
//
//  - A size or an alignment above heap::MAX_SIZE has no class, so heap::alloc()
//    stops the kernel on it. That includes the nothrow forms: nothrow means
//    "exhausted memory returns nullptr", not "an impossible request is
//    forgiven".
//  - The throwing forms panic when the heap is exhausted instead of throwing
//    std::bad_alloc. Exceptions are off (-fno-exceptions), and nothing above
//    operator new could handle an allocation failure anyway.
//  - The nothrow forms return nullptr when the heap is exhausted, which is what
//    the standard and libstdc++'s internal uses expect from them.
//
// Every delete form is one call to heap::free(): the size class lives in the
// address (see mm/heap.h), so the size and the alignment a delete-expression
// may pass are not needed, and a mismatch between a sized delete and the
// allocation would go undetected. free(nullptr) is a no-op, which operator
// delete has to accept.

#include <cstddef>
#include <new>

#include "panic.h"
#include "mm/heap.h"

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
