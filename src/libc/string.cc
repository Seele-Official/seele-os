// Minimal C string library, implemented in C++ and exported as C symbols.
//
// These are ordinary C functions at the ABI level; the extern "C" on each
// definition is what keeps the compiler from mangling the names.

#include <cstddef>
#include <cstdint>
#include <string.h>

extern "C" size_t strlen(const char* s) {
    const char* p = s;
    while(*p != '\0') {
        ++p;
    }
    return static_cast<size_t>(p - s);
}

extern "C" int strcmp(const char* a, const char* b) {
    while(*a != '\0' && *a == *b) {
        ++a;
        ++b;
    }
    const auto x = static_cast<unsigned char>(*a);
    const auto y = static_cast<unsigned char>(*b);
    return x < y ? -1 : (x > y ? 1 : 0);
}

extern "C" int strncmp(const char* a, const char* b, size_t n) {
    for(size_t i = 0; i < n; ++i) {
        const auto x = static_cast<unsigned char>(a[i]);
        const auto y = static_cast<unsigned char>(b[i]);
        if(x != y) {
            return x < y ? -1 : 1;
        }
        if(x == '\0') {
            return 0;
        }
    }
    return 0;
}

// The compiler emits calls to memcpy() for aggregate copies, so the kernel needs
// this even when no source file names it.
extern "C" void* memcpy(void* dest, const void* src, size_t count) {
    auto* out = static_cast<std::byte*>(dest);
    const auto* in = static_cast<const std::byte*>(src);
    for(size_t i = 0; i < count; ++i) {
        out[i] = in[i];
    }
    return dest;
}

extern "C" void* memmove(void* dest, const void* src, size_t count) {
    auto* out = static_cast<std::byte*>(dest);
    const auto* in = static_cast<const std::byte*>(src);
    // The ranges may overlap, so pick the copy direction from their addresses:
    // forward when dest is below src, backward otherwise.
    if(reinterpret_cast<uintptr_t>(out) < reinterpret_cast<uintptr_t>(in)) {
        for(size_t i = 0; i < count; ++i) {
            out[i] = in[i];
        }
    } else if(reinterpret_cast<uintptr_t>(out) > reinterpret_cast<uintptr_t>(in)) {
        for(size_t i = count; i != 0; --i) {
            out[i - 1] = in[i - 1];
        }
    }
    return dest;
}

extern "C" int memcmp(const void* a, const void* b, size_t count) {
    const auto* x = static_cast<const unsigned char*>(a);
    const auto* y = static_cast<const unsigned char*>(b);
    for(size_t i = 0; i < count; ++i) {
        if(x[i] != y[i]) {
            return x[i] < y[i] ? -1 : 1;
        }
    }
    return 0;
}

extern "C" void* memchr(const void* ptr, int value, size_t count) {
    const auto* bytes = static_cast<const unsigned char*>(ptr);
    const auto target = static_cast<unsigned char>(value);
    for(size_t i = 0; i < count; ++i) {
        if(bytes[i] == target) {
            return const_cast<unsigned char*>(bytes + i);
        }
    }
    return nullptr;
}

// Compiler-generated zero-initialisation of aggregates lowers to memset, so
// the kernel needs this even though no source file calls it directly.
extern "C" void* memset(void* dest, int value, size_t count) {
    auto* bytes = static_cast<std::byte*>(dest);
    for(size_t i = 0; i < count; ++i) {
        bytes[i] = static_cast<std::byte>(value);
    }
    return dest;
}
