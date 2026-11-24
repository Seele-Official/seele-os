// Platform boot discovery (platform/platform.h).

#include "platform/platform.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "console.h"
#include "panic.h"
#include "fdt/fdt.h"
#include "mm/addr.h"
#include "mm/vm.h"
#include "platform/device.h"
#include "platform/early_console.h"

namespace platform {
namespace {

// Kernel-lifetime storage owned here so `device::Device` offsets and borrowed
// string_views remain valid forever without leaking `fdt::Tree` into `kmain.cc`.
fdt::Tree g_tree;
Info g_info;

class RangeList {
public:
    RangeList(phys_range* items, size_t& count) noexcept : m_items(items), m_count(count) {}

    void add(phys_range range) noexcept {
        if(range.size == 0) {
            return;
        }
        if(!range_fits(range.base, range.size)) {
            LOG_WARN("platform: dropping region {:#018x}+{:#018x}: it wraps the address space",
                     to_uint64(range.base),
                     range.size);
            return;
        }
        if(m_count == MAX_REGIONS) {
            m_overflow = true;
            return;
        }
        m_items[m_count++] = range;
    }

    // Sort by base address and merge overlapping or adjacent ranges in place.
    void normalize() noexcept {
        m_count = normalize_ranges(std::span{m_items, m_count});
    }

    [[nodiscard]] bool overflowed() const noexcept {
        return m_overflow;
    }

private:
    phys_range* m_items;
    size_t& m_count;
    bool m_overflow = false;
};

void collect_memory(const fdt::Tree& tree,
                    Paddr dtb,
                    RangeList& usable,
                    RangeList& reserved) noexcept {
    // Reserve the DTB blob first so borrowed string_views remain backed even if
    // firmware overflows the reserved table.
    if(to_uint64(dtb) != 0 && tree.size() != 0) {
        reserved.add(phys_range{dtb, tree.size()});
    }

    tree.walk([&](fdt::Node node, std::span<const fdt::Node> ancestors) noexcept {
        if(const auto type = node.property("device_type").string(); type && *type == "memory") {
            for(const phys_range bank: node.regs(ancestors)) {
                usable.add(bank);
            }
        }
        return fdt::WalkAction::Continue;
    });

    for(const phys_range entry: tree.reserved_regions()) {
        reserved.add(entry);
    }

    if(const fdt::Node reserved_memory = tree.find("/reserved-memory")) {
        for(const fdt::Node child: reserved_memory.children()) {
            if(!child.enabled()) {
                continue;
            }
            for(const phys_range entry: child.regs()) {
                reserved.add(entry);
            }
        }
    }

    if(const fdt::Node chosen = tree.find("/chosen")) {
        const auto start = chosen.property("linux,initrd-start").u32_or_u64();
        const auto end = chosen.property("linux,initrd-end").u32_or_u64();
        if(start && end && *end > *start) {
            reserved.add(phys_range{paddr(*start), *end - *start});
        }
    }

    usable.normalize();
    reserved.normalize();
}

}  // namespace

const Info& init(Paddr dtb, uint64_t boot_hartid) noexcept {
    g_info = Info{};
    g_info.m_boot_hartid = boot_hartid;

    // Bind the DTB through its direct-map alias and attach the early console
    // before validation or discovery can log diagnostics.
    g_tree = fdt::Tree{to_pointer<const std::byte>(vm::k::P2V(dtb))};
    (void)attach_early_console(g_tree);

    SEELE_ASSERT(g_tree.valid(), "fdt: invalid device tree, no devices discovered");

    device::enumerate(g_tree);

    RangeList usable{g_info.m_memory.m_usable, g_info.m_memory.m_usable_count};
    RangeList reserved{g_info.m_memory.m_reserved, g_info.m_memory.m_reserved_count};
    collect_memory(g_tree, dtb, usable, reserved);

    bool hart_overflow = false;
    if(const fdt::Node cpus = g_tree.find("/cpus")) {
        if(const auto timebase = cpus.property("timebase-frequency").u32()) {
            g_info.m_timebase_frequency = *timebase;
        } else if(const auto root_tb = g_tree.root().property("timebase-frequency").u32()) {
            g_info.m_timebase_frequency = *root_tb;
        }

        for(const fdt::Node child: cpus.children()) {
            if(child.basename() != "cpu" || !child.enabled()) {
                continue;
            }
            const auto hartid = child.reg_address();
            if(!hartid) {
                continue;
            }
            if(g_info.m_hart_count == MAX_HARTS) {
                hart_overflow = true;
                break;
            }
            g_info.m_harts[g_info.m_hart_count++] = hart_desc{
                .hartid = *hartid,
                .isa = child.property("riscv,isa").string().value_or(std::string_view{}),
            };
        }
    }

    if(const fdt::Node chosen = g_tree.find("/chosen")) {
        g_info.m_cmdline = chosen.property("bootargs").string().value_or(std::string_view{});
    }

    if(usable.overflowed()) {
        LOG_WARN("platform: more than {} usable memory regions, the tail was dropped", MAX_REGIONS);
    }
    if(reserved.overflowed()) {
        LOG_WARN("platform: more than {} reserved memory regions, the tail was dropped",
                 MAX_REGIONS);
    }
    if(hart_overflow) {
        LOG_WARN("platform: more than {} harts, the tail was dropped", MAX_HARTS);
    }

    return g_info;
}

const Info& info() noexcept {
    return g_info;
}

}  // namespace platform
