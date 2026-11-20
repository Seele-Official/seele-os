// The platform bus: turn FDT nodes into device::Device plus its resources.
//
// This is the only file that knows both `fdt` and `device`, which is why the
// attribute accessors of Device are implemented here instead of in the header:
// drivers must not learn that the firmware describes them with a device tree.
// The tree's string_views point into the DTB, which boot::parse() keeps
// reserved, so the names stay valid for the kernel's lifetime.

#include "platform/device.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "console.h"
#include "panic.h"
#include "fdt/fdt.h"

namespace device {
namespace {

// The initial height of the /cpus tree in QEMU never exceeds a handful of
// levels; a cap keeps a malformed (cyclic-looking) structure block from
// recursing forever. Shared with the early firmware walk (early/firmware.cc)
// through one build parameter, because both walk the same tree.
#ifndef SEELE_FDT_MAX_DEPTH
#error "SEELE_FDT_MAX_DEPTH is not defined: build the kernel through CMake (see CMakeLists.txt)"
#endif
constexpr uint32_t MAX_DEPTH = SEELE_FDT_MAX_DEPTH;

Device g_devices[MAX_DEVICES];
resource g_resources[MAX_RESOURCES];
size_t g_device_count = 0;
size_t g_resource_count = 0;

// fdt::Node handle for each device, in the same order as g_devices. Device only
// stores the slot index because fdt::Node is incomplete in device.h.
fdt::Node g_nodes[MAX_DEVICES];

// phandle -> device id, for resolving `interrupt-parent`. Devices are created
// before resources are decoded, so the whole table is ready when an interrupt
// needs its controller's id.
struct phandle_entry {
    uint32_t phandle;
    uint32_t device;
};

phandle_entry g_phandles[MAX_DEVICES];
size_t g_phandle_count = 0;

[[nodiscard]] fdt::Node node_at(int slot) noexcept {
    const size_t index = static_cast<size_t>(slot);
    return index < g_device_count ? g_nodes[index] : fdt::Node{};
}

[[nodiscard]] uint32_t device_for_phandle(uint32_t phandle) noexcept {
    for(size_t i = 0; i < g_phandle_count; ++i) {
        if(g_phandles[i].phandle == phandle) {
            return g_phandles[i].device;
        }
    }
    return UINT32_MAX;
}

// An interrupt controller as seen from the device whose interrupt is being
// decoded, plus the phandle it was found through (the device id is looked up
// later, once every device exists).
struct controller_ref {
    fdt::Node node{};
    uint32_t phandle = 0;
};

// `interrupt-parent` is inherited: a node may omit it and take the value from
// its nearest ancestor that has one. QEMU virt puts it on every interrupt
// source, but the generic binding allows the bus to declare it once.
[[nodiscard]] std::optional<controller_ref> find_controller(const fdt::Tree& tree,
                                                            fdt::Node node) noexcept {
    for(fdt::Node current = node; current; current = current.parent()) {
        const fdt::Property property = current.property("interrupt-parent");
        if(!property) {
            continue;
        }
        const std::optional<uint32_t> phandle = property.u32();
        if(!phandle) {
            return std::nullopt;
        }
        fdt::Node controller = tree.by_phandle(*phandle);
        if(!controller) {
            return std::nullopt;
        }
        return controller_ref{controller, *phandle};
    }
    return std::nullopt;
}

// How many entries an `interrupts` property holds, and how wide each is.
//
// The width comes from the controller's `#interrupt-cells`: QEMU virt's PLIC
// declares 1 (just the number), the generic PLIC binding uses 2 (number plus
// trigger). Anything else (or a malformed length) yields no resources rather
// than a guess.
struct interrupt_spec {
    bool present = false;
    uint32_t cells = 0;
    uint32_t phandle = 0;
};

[[nodiscard]] interrupt_spec interrupts_of(const fdt::Tree& tree, fdt::Node node) noexcept {
    interrupt_spec result;

    const fdt::Property property = node.property("interrupts");
    if(!property || property.empty()) {
        return result;
    }

    const std::optional<controller_ref> controller = find_controller(tree, node);
    if(!controller) {
        return result;
    }

    const std::optional<uint32_t> cells = controller->node.property("#interrupt-cells").u32();
    if(!cells || (*cells != 1 && *cells != 2)) {
        return result;
    }

    const size_t entry_size = sizeof(uint32_t) * static_cast<size_t>(*cells);
    if(property.size() % entry_size != 0) {
        return result;
    }

    result.present = true;
    result.cells = *cells;
    result.phandle = controller->phandle;
    return result;
}

// The number of resources a node will contribute. The walk runs twice: once to
// size a device (so a node with no resource is never created at all), once to
// fill the resources in. Both passes must agree, which is why they share these
// helpers.
[[nodiscard]] size_t resource_count(const fdt::Tree& tree, fdt::Node node) noexcept {
    size_t count = 0;

    for(fdt::RegIterator reg = tree.reg(node);;) {
        Paddr base{};
        uint64_t size = 0;
        if(!reg.next(base, size)) {
            break;
        }
        ++count;
    }

    const interrupt_spec interrupts = interrupts_of(tree, node);
    if(interrupts.present) {
        const size_t entry_size = sizeof(uint32_t) * static_cast<size_t>(interrupts.cells);
        count += node.property("interrupts").size() / entry_size;
    }

    return count;
}

// A node becomes a device when it is enabled, names at least one compatible
// string, and owns at least one resource. The
// last condition is what excludes /soc (a simple-bus with no reg), /chosen and
// /cpus/cpu@0 (whose parent says #size-cells = 0, so its reg has no range).
[[nodiscard]] bool has_compatible(fdt::Node node) noexcept {
    std::string_view value;
    for(fdt::StringList list{node.property("compatible")}; list.next(value);) {
        return !value.empty();
    }
    return false;
}

struct candidate {
    fdt::Node node{};
    size_t resources = 0;
};

// Depth-first walk that collects every node that should become a device. It
// deliberately does not descend into /reserved-memory: those nodes describe
// memory the kernel must not hand to a driver, and they are boot::info's
// business, not the device model's.
//
// `used_resources` is the running total across the whole walk. It cannot be
// read from g_resource_count: that counter is only filled in while the devices
// are materialised, after this walk has already finished. Checking against a
// never-updated total would let a busy tree run past MAX_RESOURCES and write
// through g_resources.
void collect(const fdt::Tree& tree,
             fdt::Node node,
             uint32_t depth,
             candidate* candidates,
             size_t& count,
             size_t& used_resources) noexcept {
    if(!node) {
        return;
    }
    if(depth > MAX_DEPTH) {
        LOG_WARN("device: tree deeper than {}, truncating", MAX_DEPTH);
        return;
    }

    // /reserved-memory describes memory, not devices: its children's `reg`
    // entries are reservations boot::info already accounts for. Descending
    // would turn them into devices and let a driver touch pages the kernel must
    // keep out of the pool.
    if(node.basename() == "reserved-memory") {
        return;
    }

    if(node.enabled() && has_compatible(node)) {
        const size_t resources = resource_count(tree, node);
        if(resources != 0) {
            if(count >= MAX_DEVICES) {
                LOG_WARN("device: more than {} devices, dropping {}", MAX_DEVICES, node.name());
            } else if(used_resources + resources > MAX_RESOURCES) {
                LOG_WARN("device: more than {} resources, dropping {}", MAX_RESOURCES, node.name());
            } else {
                candidates[count++] = candidate{node, resources};
                used_resources += resources;
            }
        }
    }

    for(fdt::Node child = node.first_child(); child; child = child.next_sibling()) {
        collect(tree, child, depth + 1, candidates, count, used_resources);
    }
}

// Fill one device's window of the flat resource array.
//
// `RegIterator` already applies the parent bus's #address-cells/#size-cells and
// skips zero-length entries, so each (base, size) pair it yields is one MMIO
// resource. Interrupt resources are decoded from raw cells because their
// layout is defined by the controller, not by the node.
void decode_resources(const fdt::Tree& tree, fdt::Node node, size_t first, size_t count) noexcept {
    size_t index = first;
    const size_t end = first + count;

    for(fdt::RegIterator reg = tree.reg(node); index < end;) {
        Paddr base{};
        uint64_t size = 0;
        if(!reg.next(base, size)) {
            break;
        }
        resource& entry = g_resources[index++];
        entry = resource{};
        entry.type = ResourceType::Mmio;
        entry.mmio.base = base;
        entry.mmio.size = size;
    }

    const interrupt_spec interrupts = interrupts_of(tree, node);
    if(!interrupts.present) {
        return;
    }

    const uint32_t controller = device_for_phandle(interrupts.phandle);
    if(controller == UINT32_MAX) {
        // The parent exists in the tree but did not become a device (for
        // example an interrupt-controller with no reg). Keep the resource so the
        // counts stay consistent, but say so instead of handing a driver an
        // interrupt nobody can claim.
        LOG_WARN("device: {}: interrupt parent {:#x} is not a device",
                 node.name(),
                 interrupts.phandle);
    }
    const std::span<const std::byte> bytes = node.property("interrupts").bytes();
    const size_t entry_size = sizeof(uint32_t) * static_cast<size_t>(interrupts.cells);

    for(size_t offset = 0; index < end && offset + entry_size <= bytes.size();
        offset += entry_size) {
        resource& entry = g_resources[index++];
        entry = resource{};
        entry.type = ResourceType::Irq;
        entry.irq.controller = controller;
        entry.irq.number = fdt::be32(bytes.data() + offset);
        entry.irq.trigger = interrupts.cells == 2 ? fdt::be32(bytes.data() + offset + 4) : 0;
    }
}

}  // namespace

void enumerate(const fdt::Tree& tree) noexcept {
    g_device_count = 0;
    g_resource_count = 0;
    g_phandle_count = 0;

    if(!tree.valid()) {
        return;
    }

    candidate candidates[MAX_DEVICES];
    size_t candidate_count = 0;
    size_t used_resources = 0;
    collect(tree, tree.root(), 0, candidates, candidate_count, used_resources);
    SEELE_ASSERT(used_resources <= MAX_RESOURCES);

    // Materialise the devices first: an interrupt resource stores the device id
    // of its controller, which is only known after every device that can be a
    // controller has been created.
    for(size_t i = 0; i < candidate_count; ++i) {
        const uint32_t id = static_cast<uint32_t>(g_device_count);
        Device& device = g_devices[id];
        device = Device{};
        device.m_node = static_cast<int>(id);
        device.m_first_resource = static_cast<uint16_t>(g_resource_count);
        device.m_resource_count = static_cast<uint16_t>(candidates[i].resources);

        g_nodes[id] = candidates[i].node;
        g_device_count += 1;
        g_resource_count += candidates[i].resources;

        const std::optional<uint32_t> phandle = candidates[i].node.phandle();
        if(phandle && g_phandle_count < MAX_DEVICES) {
            g_phandles[g_phandle_count++] = phandle_entry{*phandle, id};
        }
    }

    for(size_t i = 0; i < g_device_count; ++i) {
        decode_resources(tree,
                         g_nodes[i],
                         g_devices[i].m_first_resource,
                         g_devices[i].m_resource_count);
    }
}

std::span<Device> all() noexcept {
    return std::span{g_devices, g_device_count};
}

Device* by_id(uint32_t id) noexcept {
    return id < g_device_count ? &g_devices[id] : nullptr;
}

std::string_view Device::name() const noexcept {
    const fdt::Node node = node_at(m_node);
    return node ? std::string_view{node.name()} : std::string_view{};
}

std::string_view Device::compatible() const noexcept {
    const fdt::Node node = node_at(m_node);
    std::string_view value;
    fdt::StringList list{node.property("compatible")};
    if(list.next(value)) {
        return value;
    }
    return {};
}

bool Device::is_compatible(std::string_view id) const noexcept {
    const fdt::Node node = node_at(m_node);
    std::string_view value;
    for(fdt::StringList list{node.property("compatible")}; list.next(value);) {
        if(value == id) {
            return true;
        }
    }
    return false;
}

std::span<const resource> Device::resources() const noexcept {
    return std::span{g_resources + m_first_resource, static_cast<size_t>(m_resource_count)};
}

const resource* Device::find_resource(ResourceType type, size_t index) const noexcept {
    size_t seen = 0;
    for(const resource& entry: resources()) {
        if(entry.type != type) {
            continue;
        }
        if(seen == index) {
            return &entry;
        }
        ++seen;
    }
    return nullptr;
}

std::optional<uint32_t> Device::u32_property(std::string_view name) const noexcept {
    return node_at(m_node).property(name).u32();
}

std::optional<std::string_view> Device::string_property(std::string_view name) const noexcept {
    return node_at(m_node).property(name).string();
}

}  // namespace device
