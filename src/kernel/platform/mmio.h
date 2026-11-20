#pragma once

// MMIO access for drivers.
//
// A driver never computes a pointer from a physical address: it asks for a
// `mmio::Region` and uses read()/write()/fence(). `map()` below is the single
// place where a resource's physical range becomes the virtual base of a Region.

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
// Accesses are volatile: the compiler must not fold, reorder or discard them,
// and one write must stay one store of the requested width. RISC-V does not
// order MMIO accesses beyond the weak default, so ordering relative to other
// accesses is expressed with fence(), the only ordering primitive drivers get.
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

    // Order every earlier read/write before every later one. `rw,rw` is enough
    // for device registers; the kernel does not need the stronger `io` variants
    // because it never relies on I/O ordering beyond that.
    void fence() const noexcept {
        asm volatile("fence rw,rw" ::: "memory");
    }

private:
    // Misaligned and out-of-range register accesses are driver bugs, so both
    // are checks rather than runtime errors. They are debug-only: the release
    // build must not pay for them on every character written to a UART.
    //
    // The alignment test is on the absolute address, not on the offset: a
    // region whose base is not naturally aligned makes even offset 0 misaligned
    // for the wider accesses.
    template <typename T>
    constexpr void check(uint64_t offset) const noexcept {
        SEELE_DASSERT((to_uint64(m_base) + offset) % sizeof(T) == 0);
        SEELE_DASSERT(offset <= m_size && sizeof(T) <= m_size - offset);
        // SEELE_DASSERT expands to nothing under NDEBUG, which would otherwise
        // leave the parameter unused in release builds.
        (void)offset;
    }

    Vaddr m_base{};
    uint64_t m_size = 0;
};

// Turn a device resource into an accessible region.
//
// This is the single mapping seam, and the only place a driver needs to know
// that addresses have a physical/virtual split: it converts the resource's
// Paddr into the direct-map alias with vm::k::P2V(). Drivers never call P2V()
// themselves. PAGE_OFFSET is zero today, so that alias is currently the
// physical address itself.
[[nodiscard]] inline std::optional<Region> map(const device::resource& resource) noexcept {
    if(resource.type != device::ResourceType::Mmio) {
        return std::nullopt;
    }
    if(resource.mmio.size == 0) {
        return std::nullopt;
    }

    const uint64_t base = to_uint64(resource.mmio.base);
    // Reject a range that leaves the direct map; handing it to a driver would
    // produce an address that nothing backs. The bound is the address space's,
    // so it lives in vm.h next to PAGE_OFFSET.
    if(base > vm::k::DIRECT_MAP_SIZE || resource.mmio.size > vm::k::DIRECT_MAP_SIZE - base) {
        return std::nullopt;
    }

    return Region{vm::k::P2V(resource.mmio.base), resource.mmio.size};
}

}  // namespace mmio
