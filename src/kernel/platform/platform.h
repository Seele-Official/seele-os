#pragma once

// Platform boot discovery: physical memory map, CPU topology, timebase, early
// console bring-up, and device enumeration.
//
// `platform::init()` is the single boot-time entry point that consumes the
// firmware DTB: it owns the kernel-lifetime `fdt::Tree`, attaches the early
// output console (platform/early_console.h), enumerates devices
// (platform/device.h), and populates the read-only `platform::Info`.
//
// Borrowed `std::string_view` values inside `Info` point into the DTB blob.
// They remain valid for the kernel's lifetime because `init()` records the DTB
// range in `MemoryMap::reserved()` first so `phymem` never hands those pages
// out.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "hart.h"
#include "mm/addr.h"

namespace platform {

// Fixed capacity for each sorted list (usable / reserved) in `MemoryMap`
// (-DSEELE_MAX_REGIONS). Shared with `phymem::init()` and `vm::k::init()`.
#ifndef SEELE_MAX_REGIONS
#error "SEELE_MAX_REGIONS is not defined: build the kernel through CMake (see CMakeLists.txt)"
#endif
constexpr size_t MAX_REGIONS = SEELE_MAX_REGIONS;

// Static hardware description of one enabled CPU from firmware `/cpus`.
// Runtime boot-hart identity is a single scalar (`Info::boot_hartid()`), not a
// per-hart flag.
struct hart_desc {
    uint64_t hartid = 0;
    std::string_view isa;
};

class Info;

// Sorted, non-overlapping physical memory ranges discovered from firmware,
// separated by ownership so consumers never filter on a tag enum.
class MemoryMap {
public:
    constexpr MemoryMap() noexcept = default;

    // Usable RAM banks reported by firmware (before subtracting `reserved()`).
    [[nodiscard]] std::span<const phys_range> usable() const noexcept {
        return {m_usable, m_usable_count};
    }

    // Ranges excluded from the physical page allocator (DTB blob, /memreserve/,
    // /reserved-memory, and initrd).
    [[nodiscard]] std::span<const phys_range> reserved() const noexcept {
        return {m_reserved, m_reserved_count};
    }

private:
    friend class Info;
    friend const Info& init(Paddr dtb, uint64_t boot_hartid) noexcept;

    phys_range m_usable[MAX_REGIONS]{};
    size_t m_usable_count = 0;

    phys_range m_reserved[MAX_REGIONS]{};
    size_t m_reserved_count = 0;
};

// Read-only kernel-lifetime platform state populated by `platform::init()`.
class Info {
public:
    constexpr Info() noexcept = default;

    [[nodiscard]] const MemoryMap& memory() const noexcept {
        return m_memory;
    }

    [[nodiscard]] std::span<const hart_desc> harts() const noexcept {
        return {m_harts, m_hart_count};
    }

    [[nodiscard]] constexpr uint64_t boot_hartid() const noexcept {
        return m_boot_hartid;
    }

    [[nodiscard]] constexpr uint64_t timebase_frequency() const noexcept {
        return m_timebase_frequency;
    }

    [[nodiscard]] constexpr std::string_view cmdline() const noexcept {
        return m_cmdline;
    }

private:
    friend const Info& init(Paddr dtb, uint64_t boot_hartid) noexcept;

    MemoryMap m_memory{};
    hart_desc m_harts[MAX_HARTS]{};
    size_t m_hart_count = 0;

    uint64_t m_boot_hartid = 0;
    uint64_t m_timebase_frequency = 0;
    std::string_view m_cmdline;
};

// Initialize the platform layer on the boot hart, before phymem/vm/drivers:
//   1. Bind the DTB blob into static kernel-lifetime storage.
//   2. Best-effort attach the early output console (no log/assert/alloc).
//   3. Assert the DTB is valid.
//   4. Enumerate platform devices and populate `platform::Info`.
[[nodiscard]] const Info& init(Paddr dtb, uint64_t boot_hartid) noexcept;

// Read-only view of the discovered platform topology (valid after `init()`).
[[nodiscard]] const Info& info() noexcept;

}  // namespace platform
