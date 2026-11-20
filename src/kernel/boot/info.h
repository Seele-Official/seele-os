#pragma once

// Boot-time discovery results: physical memory, harts, timebase and /chosen.
//
// boot::parse() is the only place that turns the flatten tree into kernel
// state. Everything below the platform layer consumes this struct instead of
// the FDT, so phymem never has to know that a device tree exists.
//
// The string_views point into the DTB blob. That is only safe because parse()
// also records the DTB itself as a Reserved region: the pages holding these
// strings never enter the physical page allocator. Keep this invariant when
// adding a new source of strings.

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "hart.h"
#include "fdt/fdt.h"
#include "mm/addr.h"

namespace boot {

// Upper bound for the fixed region storage below. QEMU virt needs one usable
// region and one reservation (the DTB), so this is generous; firmware that
// exceeds it gets truncated with a warning instead of a panic.
//
// A build parameter (-DSEELE_MAX_REGIONS). It is also the capacity of the range
// arrays vm::k::init() and phymem::init() build from boot::info, so raising it
// grows both the parsed result and the mapping pass.
#ifndef SEELE_MAX_REGIONS
#error "SEELE_MAX_REGIONS is not defined: build the kernel through CMake (see CMakeLists.txt)"
#endif
constexpr inline size_t MAX_REGIONS = SEELE_MAX_REGIONS;

// The hart bound lives in hart.h because hart.cc sizes its per-hart state with
// the same constant; see MAX_HARTS there.

enum class RegionKind : uint8_t {
    Usable,
    Reserved,
};

// A half-open [base, base + size) physical address range. `size` never wraps
// past the top of the address space; parse() drops such a range.
struct region {
    RegionKind kind = RegionKind::Usable;
    Paddr base = null_paddr();
    uint64_t size = 0;
};

struct hart {
    uint64_t hartid = 0;

    // Set by kmain for the hart executing it: the FDT describes the harts but
    // not which one the firmware booted, so parse() leaves every flag false.
    bool boot = false;

    // `riscv,isa` as written in the tree (the full multi-letter string).
    std::string_view isa;
};

struct info {
    region regions[MAX_REGIONS]{};
    size_t region_count = 0;

    hart harts[MAX_HARTS]{};
    size_t hart_count = 0;

    uint64_t timebase_frequency = 0;
    std::string_view cmdline;
    std::string_view stdout_path;

    Paddr dtb{};
    uint64_t dtb_size = 0;
};

// Discover everything the kernel needs from the tree.
//
// `dtb` is the physical address of the blob. fdt::Tree only stores the blob
// pointer, not its physical address, so the caller passes the address again;
// parse() records [dtb, dtb + tree.size()) as Reserved. Regions come back
// sorted by base address with same-kind neighbours merged.
[[nodiscard]] info parse(const fdt::Tree& tree, Paddr dtb) noexcept;

}  // namespace boot
