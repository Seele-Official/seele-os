#pragma once

// MMIO access for drivers and early platform sinks.
//
// Callers never compute a pointer from a physical address: they obtain an
// `mmio::Region` through `map()` and access registers via read()/write()/fence().

#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>

#include "panic.h"
#include "mm/addr.h"
#include "mm/vm.h"
#include "platform/device.h"

namespace mmio {

// A window onto a device's registers.
//
// Accesses are volatile so each read/write stays a single access of the
// requested width; ordering across registers is expressed with fence().
class Region {
public:
    constexpr Region() noexcept = default;

    constexpr Region(Vaddr base, uint64_t size) noexcept : m_base(base), m_size(size) {}

    template <typename T>
    [[nodiscard]] T read(uint64_t offset) const noexcept {
        static_assert(std::is_integral_v<T> && sizeof(T) <= 8,
                      "MMIO accesses are single integer loads or stores up to 64 bits");
        check<T>(offset);
        return *reinterpret_cast<volatile const T*>(to_uint64(m_base) + offset);
    }

    template <typename T>
    void write(uint64_t offset, T value) const noexcept {
        static_assert(std::is_integral_v<T> && sizeof(T) <= 8,
                      "MMIO accesses are single integer loads or stores up to 64 bits");
        check<T>(offset);
        *reinterpret_cast<volatile T*>(to_uint64(m_base) + offset) = value;
    }

    [[nodiscard]] constexpr Vaddr base() const noexcept {
        return m_base;
    }

    [[nodiscard]] constexpr uint64_t size() const noexcept {
        return m_size;
    }

    void fence() const noexcept {
        // Order both device I/O (i, o) and normal memory (r, w) across this point.
        asm volatile("fence iorw,iorw" ::: "memory");
    }

private:
    template <typename T>
    constexpr void check(uint64_t offset) const noexcept {
        SEELE_DASSERT((to_uint64(m_base) + offset) % sizeof(T) == 0);
        SEELE_DASSERT(offset <= m_size && sizeof(T) <= m_size - offset);
        (void)offset;
    }

    Vaddr m_base{};
    uint64_t m_size = 0;
};

// Turn a physical range into its direct-map Region. Does not create page table
// entries (device MMIO mappings are installed during `vm::k::init()`).
[[nodiscard]] inline std::optional<Region> map(const phys_range& range) noexcept {
    if(range.size == 0) {
        return std::nullopt;
    }
    const uint64_t base = to_uint64(range.base);
    if(base > vm::k::DIRECT_MAP_SIZE || range.size > vm::k::DIRECT_MAP_SIZE - base) {
        return std::nullopt;
    }
    return Region{vm::k::P2V(range.base), range.size};
}

// Convenience overload: map the `index`-th MMIO window of `device`.
[[nodiscard]] inline std::optional<Region> map(const device::Device& device,
                                               size_t index = 0) noexcept {
    const auto range = device.mmio(index);
    return range ? map(*range) : std::nullopt;
}

}  // namespace mmio
