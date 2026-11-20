// See boot/info.h. This is the only file that turns FDT properties into
// boot::info: memory banks, the reservation sources, harts, timebase and
// /chosen. Nothing here allocates; every string_view points into the blob,
// which parse() reserves as a whole.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "console.h"
#include "boot/info.h"
#include "mm/addr.h"

namespace boot {
namespace {

// Depth cap for the tree walk. A build parameter shared with the other two
// walkers (early/firmware.cc, platform/device.cc): all three read the same blob,
// so raising it in the build has to deepen all of them at once. See CMakeLists.txt.
#ifndef SEELE_FDT_MAX_DEPTH
#error "SEELE_FDT_MAX_DEPTH is not defined: build the kernel through CMake (see CMakeLists.txt)"
#endif
constexpr uint32_t MAX_TREE_DEPTH = SEELE_FDT_MAX_DEPTH;

// Collection state: the info being filled plus the overflow flags, so a huge
// tree produces one warning instead of one per dropped item.
struct collection {
    info& result;
    bool region_overflow = false;
    bool hart_overflow = false;
};

void add_region(collection& collect, Paddr base, uint64_t size, RegionKind kind) noexcept {
    if(size == 0) {
        return;
    }

    if(!range_fits(base, size)) {
        // The range wraps around the address space, so [base, base + size)
        // cannot be represented. Firmware has no business describing memory
        // that way; drop it rather than truncate it silently.
        LOG_WARN("boot: dropping region {:#018x}+{:#018x}: it wraps the address space",
                 to_uint64(base),
                 size);
        return;
    }

    if(collect.result.region_count == MAX_REGIONS) {
        collect.region_overflow = true;
        return;
    }

    collect.result.regions[collect.result.region_count++] = region{kind, base, size};
}

[[nodiscard]] bool is_memory_node(fdt::Node node) noexcept {
    const auto type = node.property("device_type").string();
    return type && *type == "memory";
}

// Every `device_type = "memory"` node can describe several banks, and every
// bank is usable. The walk does not stop at the first memory node.
void collect_memory(const fdt::Tree& tree,
                    fdt::Node node,
                    collection& collect,
                    uint32_t depth) noexcept {
    if(!node || depth > MAX_TREE_DEPTH) {
        return;
    }

    if(is_memory_node(node)) {
        fdt::RegIterator reg = tree.reg(node);
        Paddr base;
        uint64_t size = 0;
        while(reg.next(base, size)) {
            add_region(collect, base, size, RegionKind::Usable);
        }
    }

    for(fdt::Node child = node.first_child(); child; child = child.next_sibling()) {
        collect_memory(tree, child, collect, depth + 1);
    }
}

// The memory reservation block (`/memreserve/`): firmware claims these pages,
// so they must never enter the free pool.
void collect_reservation_block(const fdt::Tree& tree, collection& collect) noexcept {
    fdt::ReservedIterator reserved = tree.reserved();
    Paddr base;
    uint64_t size = 0;
    while(reserved.next(base, size)) {
        add_region(collect, base, size, RegionKind::Reserved);
    }
}

// /reserved-memory children: only enabled nodes with a `reg` describe a real
// reservation; a disabled one is a placeholder.
void collect_reserved_memory(const fdt::Tree& tree, collection& collect) noexcept {
    const fdt::Node reserved_memory = tree.find("/reserved-memory");
    if(!reserved_memory) {
        return;
    }

    for(fdt::Node child = reserved_memory.first_child(); child; child = child.next_sibling()) {
        if(!child.enabled()) {
            continue;
        }
        fdt::RegIterator reg = tree.reg(child);
        Paddr base = null_paddr();
        uint64_t size = 0;
        while(reg.next(base, size)) {
            add_region(collect, base, size, RegionKind::Reserved);
        }
    }
}

// The blob itself. Every string_view in info points into these pages, so this
// reservation is what makes those views stay valid after phymem starts handing
// pages out.
void collect_dtb(collection& collect, Paddr dtb, uint64_t size) noexcept {
    if(to_uint64(dtb) == 0 || size == 0) {
        return;
    }
    add_region(collect, dtb, size, RegionKind::Reserved);
}

// `linux,initrd-start`/`-end` are 32-bit in the original binding and 64-bit in
// the newer one; accept either. They hold physical addresses, so the decoded
// value is a Paddr: a length would be the only raw uint64_t here, and these are
// not lengths.
[[nodiscard]] std::optional<Paddr> read_address(const fdt::Property& property) noexcept {
    if(const auto value = property.u64()) {
        return paddr(*value);
    }
    if(const auto value = property.u32()) {
        return paddr(*value);
    }
    return std::nullopt;
}

void collect_initrd(const fdt::Tree& tree, collection& collect) noexcept {
    const fdt::Node chosen = tree.find("/chosen");
    if(!chosen) {
        return;
    }

    const auto start = read_address(chosen.property("linux,initrd-start"));
    const auto end = read_address(chosen.property("linux,initrd-end"));
    if(start && end && *end > *start) {
        add_region(collect, *start, *end - *start, RegionKind::Reserved);
    }
}

// The first address of a `reg` property, without its size field.
//
// RegIterator cannot be used for a CPU: /cpus has #size-cells = 0, so `reg` is
// a bare address and RegIterator (which only walks complete (base, size)
// entries) would skip it. Reading the first `#address-cells` cells directly is
// the same decoding the iterator does, just without the size half.
[[nodiscard]] std::optional<uint64_t> read_reg_address(const fdt::Tree& tree,
                                                       fdt::Node node) noexcept {
    const fdt::Property reg = node.property("reg");
    const fdt::cell_counts cells = tree.cells(node);
    if(!reg || cells.address < 1 || cells.address > 2) {
        return std::nullopt;
    }

    constexpr size_t CELL_SIZE = sizeof(uint32_t);
    const auto bytes = reg.bytes();
    if(bytes.size() < cells.address * CELL_SIZE) {
        return std::nullopt;
    }

    uint64_t address = 0;
    for(uint32_t i = 0; i < cells.address; ++i) {
        address = (address << 32) | fdt::be32(bytes.data() + i * CELL_SIZE);
    }
    return address;
}

// Harts and the timebase live under /cpus. The timebase is normally a property
// of /cpus itself; the root spelling is the legacy fallback.
void collect_cpus(const fdt::Tree& tree, collection& collect) noexcept {
    const fdt::Node cpus = tree.find("/cpus");
    if(!cpus) {
        return;
    }

    if(const auto timebase = cpus.property("timebase-frequency").u32()) {
        collect.result.timebase_frequency = *timebase;
    } else if(const auto timebase = tree.root().property("timebase-frequency").u32()) {
        collect.result.timebase_frequency = *timebase;
    }

    for(fdt::Node child = cpus.first_child(); child; child = child.next_sibling()) {
        if(child.basename() != "cpu") {
            continue;  // cpu-map and its cluster/core children are topology
        }
        // A disabled CPU is not schedulable: count it as absent rather than
        // advertising a hart the kernel can never start.
        if(!child.enabled()) {
            continue;
        }

        const auto hartid = read_reg_address(tree, child);
        if(!hartid) {
            continue;
        }

        if(collect.result.hart_count == MAX_HARTS) {
            collect.hart_overflow = true;
            break;
        }

        hart& entry = collect.result.harts[collect.result.hart_count++];
        entry.hartid = *hartid;
        entry.boot = false;
        if(const auto isa = child.property("riscv,isa").string()) {
            entry.isa = *isa;
        }
    }
}

void collect_chosen(const fdt::Tree& tree, info& result) noexcept {
    const fdt::Node chosen = tree.find("/chosen");
    if(!chosen) {
        return;
    }

    if(const auto bootargs = chosen.property("bootargs").string()) {
        result.cmdline = *bootargs;
    }
    if(const auto stdout_path = chosen.property("stdout-path").string()) {
        // Kept verbatim: the ":115200n8" suffix is part of the console
        // description, and callers that care can strip it.
        result.stdout_path = *stdout_path;
    }
}

// Insertion sort by base address: the number of regions is tiny and no heap is
// available. Equal bases keep their collection order.
void sort_regions(info& table) noexcept {
    for(size_t i = 1; i < table.region_count; ++i) {
        const region key = table.regions[i];
        size_t j = i;
        while(j > 0 && to_uint64(table.regions[j - 1].base) > to_uint64(key.base)) {
            table.regions[j] = table.regions[j - 1];
            --j;
        }
        table.regions[j] = key;
    }
}

// Merge overlapping or adjacent regions of the same kind. Different kinds are
// never merged: a reservation inside a usable bank has to stay visible as its
// own entry so phymem can subtract it.
//
// Regions arrive sorted by base, so a region can only overlap an output entry
// that starts at or below it. That is not the same as "the previous output
// entry": usable banks A and B separated by a reservation (A, R, B) can still
// overlap each other, and merging only neighbours would leave both in the list.
// phymem::init() would then push the pages of the shared part twice -- the same
// physical page reachable from the free list twice, i.e. handed to two owners --
// which is why the scan is over the whole output list.
void merge_regions(info& table) noexcept {
    size_t out = 0;
    for(size_t i = 0; i < table.region_count; ++i) {
        const region candidate = table.regions[i];
        bool absorbed = false;
        for(size_t j = 0; j < out; ++j) {
            region& existing = table.regions[j];
            if(existing.kind != candidate.kind) {
                continue;
            }
            const Paddr existing_end = range_end(existing.base, existing.size);
            if(candidate.base > existing_end) {
                // Disjoint from this entry. A later output entry starts higher
                // but may still reach up to `candidate`, so keep scanning.
                continue;
            }
            // Sorted by base, so `candidate` cannot start below `existing`; the
            // union is [existing.base, max(existing_end, candidate_end)).
            const Paddr candidate_end = range_end(candidate.base, candidate.size);
            if(candidate_end > existing_end) {
                existing.size = candidate_end - existing.base;
            }
            absorbed = true;
            break;
        }
        if(!absorbed) {
            table.regions[out++] = candidate;
        }
    }
    table.region_count = out;
}

}  // namespace

info parse(const fdt::Tree& tree, Paddr dtb) noexcept {
    info result;
    result.dtb = dtb;

    if(!tree.valid()) {
        LOG_WARN("boot: invalid device tree, no memory discovered");
        return result;
    }
    result.dtb_size = tree.size();

    collection collect{result};

    // The DTB is recorded first, before any region the firmware describes. Every
    // string_view in info points into the blob, so its reservation is the one
    // entry that must never be the one a full table drops; the list is sorted
    // later, so the order of collection has no other effect.
    collect_dtb(collect, dtb, tree.size());
    collect_memory(tree, tree.root(), collect, 0);
    collect_reservation_block(tree, collect);
    collect_reserved_memory(tree, collect);
    collect_initrd(tree, collect);
    collect_cpus(tree, collect);
    collect_chosen(tree, result);

    sort_regions(result);
    merge_regions(result);

    if(collect.region_overflow) {
        LOG_WARN("boot: more than {} regions, the tail was dropped", MAX_REGIONS);
    }
    if(collect.hart_overflow) {
        LOG_WARN("boot: more than {} harts, the tail was dropped", MAX_HARTS);
    }
    return result;
}

}  // namespace boot
