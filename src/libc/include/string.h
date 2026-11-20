#pragma once

// Minimal C string functions provided by the kernel's own C library.
//
// The definitions live in src/libc/string.cc. They are compiled as C++ but
// exported with C linkage (see the extern "C" block there), so the symbols are
// the plain strlen/strcmp/strncmp that freestanding code expects.
//
// Only what the kernel actually uses is declared here. Add mem*/str* functions
// when a caller needs them, not preemptively.

#include <stddef.h>

extern "C" {

    size_t strlen(const char* s);
    int strcmp(const char* a, const char* b);
    int strncmp(const char* a, const char* b, size_t n);
    void* memcpy(void* dest, const void* src, size_t count);
    void* memmove(void* dest, const void* src, size_t count);
    void* memset(void* dest, int value, size_t count);
    int memcmp(const void* a, const void* b, size_t count);
    void* memchr(const void* ptr, int value, size_t count);
}
