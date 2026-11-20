#pragma once

// Physical and virtual address types.
//
// A Paddr and a Vaddr are both 64-bit values with the same representation, but
// they name different things: the kernel's direct map is identity, so most
// kernel Vaddrs happen to equal their Paddr today, while kernel stacks also
// have a high alias that is not a direct-map address. Keeping the types
// distinct means the two classic mix-ups -- a physical address dereferenced as
// a pointer, or an MMIO base stored as a virtual address -- stop compiling
// instead of surviving until the address space changes.
//
// This header is representation and address-space-neutral arithmetic only. It
// deliberately knows no address space: a Vaddr is any virtual address, without
// implying which address space it names. Turning a Paddr into something that
// can be dereferenced is therefore policy, and it lives next to the layout that
// makes it true -- vm::k::P2V() / vm::k::V2P() in vm.h for the kernel direct
// map.
//
// The page-alignment and overlap helpers at the bottom know no address space
// either: they are pure 64-bit arithmetic, which is why both the physical
// allocator (rounding boot regions) and the page-table code (rounding the
// ranges it maps) can use them. They are here rather than in vm.h because
// vm.h is where layout policy lives.

#include <concepts>
#include <cstddef>
#include <cstdint>

#include "riscv.h"

enum class Paddr : uint64_t {};
enum class Vaddr : uint64_t {};

// ---------------------------------------------------------------------------
// Typed address helpers
// ---------------------------------------------------------------------------
//
// Everything below accepts only a Paddr or a Vaddr, never a raw integer: the
// two address types are distinct even while they are numerically equal, and the
// compiler should reject a mix-up at the call site instead of a review catching
// it. Construct the address at the boundary with paddr()/vaddr().
//
// The helpers know no address space: they are pure 64-bit arithmetic, which is
// why the physical allocator (rounding boot regions) and the page-table code
// (rounding the ranges it maps) can share them.

template <typename T>
concept address_like = std::same_as<T, Paddr> || std::same_as<T, Vaddr>;

// Explicit constructors from the raw value, and back. There is deliberately no
// implicit conversion between Paddr, Vaddr and integers: every crossing is
// spelled out, which is the entire point of the two types.
[[nodiscard]] constexpr Paddr paddr(uint64_t value) noexcept {
    return static_cast<Paddr>(value);
}

[[nodiscard]] constexpr Vaddr vaddr(uint64_t value) noexcept {
    return static_cast<Vaddr>(value);
}

// A pointer names a virtual address, so there is a vaddr() overload for it and
// deliberately no paddr() one: turning a pointer into a physical address is
// V2P(), which only the layout that makes it true (the direct map) may define.
// The missing overload is what keeps `paddr(some_pointer)` from compiling.
[[nodiscard]] inline Vaddr vaddr(const void* value) noexcept {
    return vaddr(reinterpret_cast<uintptr_t>(value));
}

[[nodiscard]] constexpr uint64_t to_uint64(Paddr address) noexcept {
    return static_cast<uint64_t>(address);
}

[[nodiscard]] constexpr uint64_t to_uint64(Vaddr address) noexcept {
    return static_cast<uint64_t>(address);
}

// The other end of vaddr(const void*): turn a Vaddr back into a pointer.
//
// Only correct for an address that already names mapped memory -- a direct-map
// alias (vm::k::P2V()), a kernel-image address or a high window slot -- because
// the type says nothing about whether anything is mapped there. That is why the
// conversion is here rather than being open-coded at every use: the shape
// `reinterpret_cast<T*>(to_uint64(...))` is the "dereference a bare integer"
// pattern this header exists to make visible.
template <typename T>
[[nodiscard]] inline T* to_pointer(Vaddr address) noexcept {
    return reinterpret_cast<T*>(to_uint64(address));
}

// The null address. A value of 0 is what phymem::alloc() returns when it has no
// page left, so "no address" needs one spelling that reads as a value rather
// than as a physical address: constructing it from the underlying type says
// "nothing here", where paddr(0) would say "physical page zero".
[[nodiscard]] constexpr Paddr null_paddr() noexcept {
    return Paddr{};
}

// True when `address` is the null address (null_paddr()), i.e. what alloc()
// returns when it has no page left.
[[nodiscard]] constexpr bool is_null(Paddr address) noexcept {
    return to_uint64(address) == 0;
}

// An address plus a byte offset. Two addresses cannot be added: the sum of two
// Paddrs does not name a physical address, and the compiler refusing that is
// cheaper than a code review catching it.
//
// The result wraps modulo 2^64 when the sum runs past the top of the address
// space, because an address the hardware cannot represent is the caller's bug to
// avoid -- except when the sum is the upper bound of a range, where range_end()
// saturates instead and is the one to use.
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

// The distance between two addresses: `a - b` is the number of bytes from `b` to
// `a`, a plain count rather than an address. Adding two addresses is still
// rejected, but their difference is exactly what a range end minus its begin is,
// and spelling it this way keeps the intent (a length) separate from the
// representation (two uint64_t conversions).
//
// Both operands must be the same kind of address: a Paddr minus a Vaddr is not a
// distance in any single address space, so it fails to deduce instead of
// compiling. The result wraps when `a < b`; the caller guarantees the order, the
// same way it does for `end - begin` on a range it built.
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

// Round `address` up to the next page boundary (riscv::PGSIZE), or down to the
// previous one. Only whole pages can be allocated or mapped, so these are the
// two pieces of mask arithmetic the allocator and the page tables share.
//
// align_up() wraps to 0 at the top of the address space instead of saturating:
// there is no page above the top, and a caller that can be handed such a range
// has a range-validity problem to handle, not an alignment one. phymem::init()
// checks the result for exactly that reason.
template <address_like Address>
[[nodiscard]] constexpr Address align_up(Address address) noexcept {
    return static_cast<Address>((to_uint64(address) + riscv::PGOFFSET_MASK) &
                                ~riscv::PGOFFSET_MASK);
}

template <address_like Address>
[[nodiscard]] constexpr Address align_down(Address address) noexcept {
    return static_cast<Address>(to_uint64(address) & ~riscv::PGOFFSET_MASK);
}

// True when `size` is a whole number of pages. A byte count is not an address,
// so this one deliberately takes a length instead of a Paddr/Vaddr.
[[nodiscard]] constexpr bool is_page_multiple(uint64_t size) noexcept {
    return (size & riscv::PGOFFSET_MASK) == 0;
}

template <address_like Address>
[[nodiscard]] constexpr bool is_page_aligned(Address address) noexcept {
    return is_page_multiple(to_uint64(address));
}

// ---------------------------------------------------------------------------
// Half-open byte ranges
// ---------------------------------------------------------------------------
//
// A range is [begin, begin + size), the same convention boot::region and the
// page-table walks use. The endpoints are addresses, so they keep their Paddr
// or Vaddr type; only the length is a raw count.

// True when [begin, begin + size) fits below the top of the address space, i.e.
// when range_end() would not have to saturate. A region that fails this has no
// addressable end, so a caller that receives one from firmware or a configuration
// file should drop it instead of clamping it -- which is exactly what
// boot::add_region() does.
template <address_like Address>
[[nodiscard]] constexpr bool range_fits(Address begin, uint64_t size) noexcept {
    return size <= UINT64_MAX - to_uint64(begin);
}

// The end of [begin, begin + size). The end saturates at the top of the address
// space instead of wrapping, so a range that reaches the top cannot turn into
// an empty or inverted range that silently passes every overlap test. A size of
// zero yields the empty range [begin, begin).
template <address_like Address>
[[nodiscard]] constexpr Address range_end(Address begin, uint64_t size) noexcept {
    return static_cast<Address>(range_fits(begin, size) ? to_uint64(begin) + size : UINT64_MAX);
}

// True when [a_begin, a_end) and [b_begin, b_end) share at least one byte. An
// empty range overlaps nothing, even when it sits strictly inside the other
// range, so `size == 0` callers need no special case.
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

// True when the page [page, page + PGSIZE) shares a byte with [begin, begin +
// size), i.e. whether allocating or mapping that page would touch the range.
// `page` must be page aligned; `size == 0` touches nothing.
template <address_like Address>
[[nodiscard]] constexpr bool page_overlaps(Address page, Address begin, uint64_t size) noexcept {
    return ranges_overlap(page, range_end(page, riscv::PGSIZE), begin, range_end(begin, size));
}
