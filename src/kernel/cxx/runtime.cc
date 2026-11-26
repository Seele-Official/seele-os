// Minimal C++ ABI runtime for the freestanding libstdc++ subset.
//
// This file defines ONLY C++ implementation symbols: the libstdc++ ABI surface
// and the C++ panic path.  It deliberately does not define any C library
// symbol.  memcpy, memset, abort, malloc, ... are the kernel libc's job.
//
// It also does not define operator new/delete.  Those are the kernel heap's:
// mm/heap.cc defines every replaceable form as a wrapper over heap::alloc() /
// heap::free(), so a `new` expression has the heap's size limit (MAX_SIZE) and
// panics when memory runs out.  Note that the deleting destructors of
// std::exception/std::bad_exception reference operator delete, which the heap
// provides, so those types can be used.
//
// The symbols defined here are:
//   std::nothrow
//   __cxa_pure_virtual / __cxa_deleted_virtual
//   __cxx_panic (weak)
//
// std::exception / std::bad_exception need nothing here: libstdc++ 16.2 makes
// their members inline in C++26.  std::terminate / std::set_terminate /
// std::get_terminate are only declared below; they are planned for when
// exceptions are enabled (terminate -> panic, handlers in the hart context).

#include <exception>
#include <new>

extern "C" {
    [[noreturn]] void panic(const char* message);

    // C++ runtime panic hook. This weak definition forwards directly to
    // panic(message) so the kernel halts with diagnostics, while still allowing
    // a strong override if needed. It is a C symbol, but belongs to the C++
    // runtime, not libc.
    __attribute__((weak)) void __cxx_panic(const char* message) {
        panic(message);
    }
}

// ---------------------------------------------------------------------------
// <exception> ABI surface
// ---------------------------------------------------------------------------

// Declarations only, for now: these are part of the plan for when exceptions
// are enabled (terminate -> panic, handlers stored per hart).  A caller today
// would fail to link.
namespace std {

terminate_handler set_terminate(terminate_handler handler) noexcept;

terminate_handler get_terminate() noexcept;

void terminate() noexcept;

int uncaught_exceptions() noexcept;

// std::nothrow is normally provided by libsupc++.  It is a plain data object
// and does not pull in any libc dependency, so define it here.
const nothrow_t nothrow{};

}  // namespace std

// ---------------------------------------------------------------------------
// Compiler-generated C++ ABI hooks
// ---------------------------------------------------------------------------

extern "C" {

    [[noreturn]] void __cxa_pure_virtual(void) {
        panic("pure virtual function called");
    }

    [[noreturn]] void __cxa_deleted_virtual(void) {
        panic("deleted virtual function called");
    }

}  // extern "C"
