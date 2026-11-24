#pragma once

// Strong physical (Paddr) and virtual (Vaddr) address types, plus
// address-space-neutral alignment and half-open range helpers.
//
// Paddr and Vaddr are distinct even while the direct map is identity: every
// integer/pointer crossing is explicit, and Paddr <-> Vaddr policy lives in
// mm/vm.h (vm::k::P2V / V2P).

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>

#include "riscv.h"

enum class Paddr : uint64_t {};
enum class Vaddr : uint64_t {};

template <typename T>
concept address_like = std::same_as<T, Paddr> || std::same_as<T, Vaddr>;

[[nodiscard]] constexpr Paddr paddr(uint64_t value) noexcept {
    return static_cast<Paddr>(value);
}

[[nodiscard]] constexpr Vaddr vaddr(uint64_t value) noexcept {
    return static_cast<Vaddr>(value);
}

// Pointers name virtual addresses; converting a pointer to a Paddr requires
// vm::k::V2P(), so there is deliberately no paddr(const void*) overload.
[[nodiscard]] inline Vaddr vaddr(const void* value) noexcept {
    return vaddr(reinterpret_cast<uintptr_t>(value));
}

[[nodiscard]] constexpr uint64_t to_uint64(Paddr address) noexcept {
    return static_cast<uint64_t>(address);
}

[[nodiscard]] constexpr uint64_t to_uint64(Vaddr address) noexcept {
    return static_cast<uint64_t>(address);
}

// Convert a Vaddr back into a pointer. Caller must ensure `address` names
// mapped memory (direct-map alias, kernel image, or high-window slot).
template <typename T>
[[nodiscard]] inline T* to_pointer(Vaddr address) noexcept {
    return reinterpret_cast<T*>(to_uint64(address));
}

// Null physical address sentinel (e.g. phymem::alloc() exhaustion), distinct in
// intent from paddr(0).
[[nodiscard]] constexpr Paddr null_paddr() noexcept {
    return Paddr{};
}

[[nodiscard]] constexpr bool is_null(Paddr address) noexcept {
    return to_uint64(address) == 0;
}

// Address +/- byte offset (wraps modulo 2^64; use range_end() for saturated
// range upper bounds). Adding two addresses is intentionally disallowed.
[[nodiscard]] constexpr Paddr operator+ (Paddr address, uint64_t offset) noexcept {
    return paddr(to_uint64(address) + offset);
}

[[nodiscard]] constexpr Paddr operator- (Paddr address, uint64_t offset) noexcept {
    return paddr(to_uint64(address) - offset);
}

[[nodiscard]] constexpr Vaddr operator+ (Vaddr address, uint64_t offset) noexcept {
    return vaddr(to_uint64(address) + offset);
}

[[nodiscard]] constexpr Vaddr operator- (Vaddr address, uint64_t offset) noexcept {
    return vaddr(to_uint64(address) - offset);
}

// Byte distance between two addresses of the same kind; caller guarantees a >= b.
template <address_like Address>
[[nodiscard]] constexpr uint64_t operator- (Address a, Address b) noexcept {
    return to_uint64(a) - to_uint64(b);
}

template <address_like Address>
constexpr Address& operator-= (Address& address, uint64_t offset) noexcept {
    address = address - offset;
    return address;
}

template <address_like Address>
constexpr Address& operator+= (Address& address, uint64_t offset) noexcept {
    address = address + offset;
    return address;
}

// ---------------------------------------------------------------------------
// Page alignment
// ---------------------------------------------------------------------------

// Round to a riscv::PGSIZE boundary. align_up() wraps to 0 past UINT64_MAX;
// callers handling untrusted ranges must check for wrap.
template <address_like Address>
[[nodiscard]] constexpr Address align_up(Address address) noexcept {
    return static_cast<Address>((to_uint64(address) + riscv::PGOFFSET_MASK) &
                                ~riscv::PGOFFSET_MASK);
}

template <address_like Address>
[[nodiscard]] constexpr Address align_down(Address address) noexcept {
    return static_cast<Address>(to_uint64(address) & ~riscv::PGOFFSET_MASK);
}

[[nodiscard]] constexpr bool is_page_multiple(uint64_t size) noexcept {
    return (size & riscv::PGOFFSET_MASK) == 0;
}

template <address_like Address>
[[nodiscard]] constexpr bool is_page_aligned(Address address) noexcept {
    return is_page_multiple(to_uint64(address));
}

// ---------------------------------------------------------------------------
// Half-open byte ranges [base, base + size)
// ---------------------------------------------------------------------------

struct phys_range {
    Paddr base = null_paddr();
    uint64_t size = 0;
};

// True when [begin, begin + size) does not overflow UINT64_MAX.
template <address_like Address>
[[nodiscard]] constexpr bool range_fits(Address begin, uint64_t size) noexcept {
    return size <= UINT64_MAX - to_uint64(begin);
}

// End of [begin, begin + size), saturating at UINT64_MAX instead of wrapping.
template <address_like Address>
[[nodiscard]] constexpr Address range_end(Address begin, uint64_t size) noexcept {
    return static_cast<Address>(range_fits(begin, size) ? to_uint64(begin) + size : UINT64_MAX);
}

// True when [a_begin, a_end) and [b_begin, b_end) share at least one byte;
// empty ranges overlap nothing.
template <address_like Address>
[[nodiscard]] constexpr bool
    ranges_overlap(Address a_begin, Address a_end, Address b_begin, Address b_end) noexcept {
    const uint64_t a_first = to_uint64(a_begin);
    const uint64_t a_last = to_uint64(a_end);
    const uint64_t b_first = to_uint64(b_begin);
    const uint64_t b_last = to_uint64(b_end);
    return a_first < a_last && b_first < b_last && a_first < b_last && b_first < a_last;
}

// True when `address` lies in [begin, end).
template <address_like Address>
[[nodiscard]] constexpr bool range_contains(Address begin, Address end, Address address) noexcept {
    const uint64_t value = to_uint64(address);
    return to_uint64(begin) <= value && value < to_uint64(end);
}

// True when page [page, page + PGSIZE) overlaps [begin, begin + size).
// `page` must be page aligned; `size == 0` overlaps nothing.
template <address_like Address>
[[nodiscard]] constexpr bool page_overlaps(Address page, Address begin, uint64_t size) noexcept {
    return ranges_overlap(page, range_end(page, riscv::PGSIZE), begin, range_end(begin, size));
}

// Sort `ranges` by base address and merge overlapping or adjacent intervals in
// place. Returns the number of merged ranges written to the prefix of `ranges`.
[[nodiscard]] constexpr size_t normalize_ranges(std::span<phys_range> ranges) noexcept {
    for(size_t i = 1; i < ranges.size(); ++i) {
        const phys_range key = ranges[i];
        size_t j = i;
        while(j > 0 && ranges[j - 1].base > key.base) {
            ranges[j] = ranges[j - 1];
            --j;
        }
        ranges[j] = key;
    }

    size_t out = 0;
    for(const phys_range& candidate: ranges) {
        if(out > 0) {
            phys_range& prev = ranges[out - 1];
            const Paddr prev_end = range_end(prev.base, prev.size);
            if(candidate.base <= prev_end) {
                const Paddr cand_end = range_end(candidate.base, candidate.size);
                if(cand_end > prev_end) {
                    prev.size = cand_end - prev.base;
                }
                continue;
            }
        }
        ranges[out++] = candidate;
    }
    return out;
}
