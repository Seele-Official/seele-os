// The platform bus: turn FDT nodes into device::Device with typed MMIO and IRQ
// pools (platform/device.h).

#include "platform/device.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "console.h"
#include "fdt/fdt.h"

namespace device {
namespace {

const fdt::Tree* g_tree = nullptr;
Device g_devices[MAX_DEVICES];
phys_range g_mmios[MAX_RESOURCES];
irq_line g_irqs[MAX_RESOURCES];
size_t g_device_count = 0;
size_t g_mmio_count = 0;
size_t g_irq_count = 0;

struct device_phandle {
    uint32_t phandle;
    uint32_t device;
};

struct controller_ref {
    fdt::Node node{};
    uint32_t phandle = 0;
};

[[nodiscard]] fdt::Node node_of(uint32_t offset) noexcept {
    return g_tree != nullptr ? g_tree->node_at(offset) : fdt::Node{};
}

[[nodiscard]] uint32_t device_for_phandle(std::span<const device_phandle> table,
                                          uint32_t phandle) noexcept {
    for(const device_phandle& entry: table) {
        if(entry.phandle == phandle) {
            return entry.device;
        }
    }
    return UINT32_MAX;
}

// Resolve `interrupt-parent` from `node` or its nearest ancestor in `ancestors`.
[[nodiscard]] std::optional<controller_ref>
    find_controller(const fdt::Tree& tree,
                    fdt::Node node,
                    std::span<const fdt::Node> ancestors) noexcept {
    auto resolve = [&tree](fdt::Node current) noexcept -> std::optional<controller_ref> {
        const fdt::Property prop = current.property("interrupt-parent");
        if(!prop) {
            return std::nullopt;
        }
        const auto phandle = prop.u32();
        if(!phandle) {
            return controller_ref{};
        }
        const fdt::Node controller = tree.by_phandle(*phandle);
        return controller ? controller_ref{controller, *phandle} : controller_ref{};
    };

    if(const auto found = resolve(node)) {
        return found->node ? found : std::nullopt;
    }
    for(size_t i = ancestors.size(); i > 0; --i) {
        if(const auto found = resolve(ancestors[i - 1])) {
            return found->node ? found : std::nullopt;
        }
    }
    return std::nullopt;
}

[[nodiscard]] bool has_compatible(fdt::Node node) noexcept {
    for(const std::string_view value: node.property("compatible").strings()) {
        (void)value;
        return true;
    }
    return false;
}

// Decode MMIO and IRQ entries for `node` into the flat typed pools, recording
// each IRQ's parent phandle for post-walk controller resolution. Returns false
// when either resource pool overflows (caller rewinds both counts).
[[nodiscard]] bool append_resources(const fdt::Tree& tree,
                                    fdt::Node node,
                                    std::span<const fdt::Node> ancestors,
                                    device_phandle* irq_bindings,
                                    uint32_t device_id) noexcept {
    for(const phys_range range: node.regs(ancestors)) {
        if(g_mmio_count == MAX_RESOURCES) {
            return false;
        }
        g_mmios[g_mmio_count++] = range;
    }

    const fdt::Property interrupts = node.property("interrupts");
    if(!interrupts || interrupts.empty()) {
        return true;
    }
    const auto controller = find_controller(tree, node, ancestors);
    if(!controller) {
        return true;
    }
    const auto cells = controller->node.property("#interrupt-cells").u32();
    if(!cells || (*cells != 1 && *cells != 2)) {
        return true;
    }

    const size_t entry_size = sizeof(uint32_t) * static_cast<size_t>(*cells);
    const auto bytes = interrupts.bytes();
    if(bytes.size() % entry_size != 0) {
        return true;
    }

    for(size_t offset = 0; offset + entry_size <= bytes.size(); offset += entry_size) {
        if(g_irq_count == MAX_RESOURCES) {
            return false;
        }
        irq_bindings[g_irq_count] = device_phandle{controller->phandle, device_id};
        g_irqs[g_irq_count++] = irq_line{
            .controller = UINT32_MAX,
            .number = fdt::be32(bytes.data() + offset),
            .trigger = *cells == 2 ? fdt::be32(bytes.data() + offset + 4) : 0,
        };
    }
    return true;
}

}  // namespace

void enumerate(const fdt::Tree& tree) noexcept {
    g_tree = &tree;
    g_device_count = 0;
    g_mmio_count = 0;
    g_irq_count = 0;

    if(!tree.valid()) {
        return;
    }

    device_phandle phandles[MAX_DEVICES]{};
    size_t phandle_count = 0;
    device_phandle irq_bindings[MAX_RESOURCES]{};
    bool truncated = false;

    tree.walk(
        [&](fdt::Node node, std::span<const fdt::Node> ancestors) noexcept {
            if(node.basename() == "reserved-memory") {
                return fdt::WalkAction::SkipChildren;
            }
            if(!node.enabled() || !has_compatible(node)) {
                return fdt::WalkAction::Continue;
            }

            const size_t mmio_begin = g_mmio_count;
            const size_t irq_begin = g_irq_count;
            const uint32_t candidate_id = static_cast<uint32_t>(g_device_count);

            if(!append_resources(tree, node, ancestors, irq_bindings, candidate_id)) {
                g_mmio_count = mmio_begin;
                g_irq_count = irq_begin;
                LOG_WARN("device: more than {} resources, dropping {}", MAX_RESOURCES, node.name());
                return fdt::WalkAction::Continue;
            }

            const size_t mmio_count = g_mmio_count - mmio_begin;
            const size_t irq_count = g_irq_count - irq_begin;
            if(mmio_count == 0 && irq_count == 0) {
                return fdt::WalkAction::Continue;
            }
            if(g_device_count == MAX_DEVICES) {
                g_mmio_count = mmio_begin;
                g_irq_count = irq_begin;
                LOG_WARN("device: more than {} devices, dropping {}", MAX_DEVICES, node.name());
                return fdt::WalkAction::Continue;
            }

            Device& dev = g_devices[g_device_count++];
            dev = Device{};
            dev.m_fdt_offset = node.offset();
            dev.m_mmio_begin = static_cast<uint16_t>(mmio_begin);
            dev.m_mmio_count = static_cast<uint16_t>(mmio_count);
            dev.m_irq_begin = static_cast<uint16_t>(irq_begin);
            dev.m_irq_count = static_cast<uint16_t>(irq_count);

            if(const auto phandle = node.phandle()) {
                phandles[phandle_count++] = device_phandle{*phandle, candidate_id};
            }
            return fdt::WalkAction::Continue;
        },
        &truncated);

    if(truncated) {
        LOG_WARN("device: tree deeper than {}, truncating", fdt::MAX_DEPTH);
    }

    // Resolve each IRQ line's interrupt-parent phandle to its controller device
    // id now that every device exists.
    const std::span<const device_phandle> phandle_table{phandles, phandle_count};
    uint32_t last_warned_device = UINT32_MAX;
    for(size_t i = 0; i < g_irq_count; ++i) {
        const uint32_t controller = device_for_phandle(phandle_table, irq_bindings[i].phandle);
        g_irqs[i].controller = controller;
        if(controller == UINT32_MAX && irq_bindings[i].device != last_warned_device) {
            last_warned_device = irq_bindings[i].device;
            LOG_WARN("device: {}: interrupt parent {:#x} is not a device",
                     g_devices[irq_bindings[i].device].name(),
                     irq_bindings[i].phandle);
        }
    }
}

std::span<Device> all() noexcept {
    return std::span{g_devices, g_device_count};
}

Device* by_id(uint32_t id) noexcept {
    return id < g_device_count ? &g_devices[id] : nullptr;
}

std::string_view Device::name() const noexcept {
    return node_of(m_fdt_offset).name();
}

std::string_view Device::compatible() const noexcept {
    for(const std::string_view value: node_of(m_fdt_offset).property("compatible").strings()) {
        return value;
    }
    return {};
}

bool Device::is_compatible(std::string_view id) const noexcept {
    return node_of(m_fdt_offset).is_compatible(id);
}

std::span<const phys_range> Device::mmios() const noexcept {
    return std::span{g_mmios + m_mmio_begin, static_cast<size_t>(m_mmio_count)};
}

std::span<const irq_line> Device::irqs() const noexcept {
    return std::span{g_irqs + m_irq_begin, static_cast<size_t>(m_irq_count)};
}

std::optional<phys_range> Device::mmio(size_t index) const noexcept {
    const auto list = mmios();
    return index < list.size() ? std::optional<phys_range>{list[index]} : std::nullopt;
}

std::optional<irq_line> Device::irq(size_t index) const noexcept {
    const auto list = irqs();
    return index < list.size() ? std::optional<irq_line>{list[index]} : std::nullopt;
}

std::optional<uint32_t> Device::u32_property(std::string_view name) const noexcept {
    return node_of(m_fdt_offset).property(name).u32();
}

std::optional<std::string_view> Device::string_property(std::string_view name) const noexcept {
    return node_of(m_fdt_offset).property(name).string();
}

}  // namespace device
