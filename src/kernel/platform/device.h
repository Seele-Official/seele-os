#pragma once

// The device model: what the firmware says exists, in kernel terms.
//
// The FDT is a byte parser (fdt/fdt.h) and drivers must not see it. This
// header is the seam in between: `platform/device.cc` turns tree nodes into
// `device::Device` and `device::resource`, and a driver only ever consumes
// resources and attributes. That is why nothing here mentions `fdt` beyond an
// opaque tree pointer: the type is forward declared, the header does not
// include it, and the attribute accessors are implemented in the bus (where
// including the parser is allowed).
//
// Storage is entirely static: an array of devices plus one flat array of
// resources, each device holding a (first, count) window into it. There is no
// heap, no allocation order to get wrong, and a device id is just an index into
// `all()`.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "device_object.h"
#include "mm/addr.h"

namespace fdt {
class Tree;
}

namespace device {

enum class ResourceType : uint8_t {
    Mmio,
    Irq,
};

struct mmio_range {
    Paddr base;
    uint64_t size;
};

struct irq_line {
    uint32_t controller;  // device id of the interrupt controller
    uint32_t number;      // interrupt number in that controller
    uint32_t trigger;     // 1=rising 2=falling 3=level-high 4=level-low, 0=unknown
};

// One address range or one interrupt line a device owns.
//
// The union keeps the flat resource array small and, more importantly, makes
// "a device has a list of resources of different kinds" a single type instead
// of a per-device-kind struct (the `platform::Config` mistake this replaces).
struct resource {
    ResourceType type;

    union {
        mmio_range mmio;
        irq_line irq;
    };
};

// Upper bounds for the static storage. QEMU virt describes 16 devices and 27
// resources, so these are generous. Overflow is truncated with a warning, not
// a panic: firmware that gives more devices than expected should not kill the
// kernel.
//
// Build parameters (-DSEELE_MAX_DEVICES, -DSEELE_MAX_RESOURCES): they are the
// capacity of the bus's static arrays and of the range arrays vm::k::init()
// builds from them, so a board with more devices raises them (and pays in
// .bss) at configure time.
#ifndef SEELE_MAX_DEVICES
#error "SEELE_MAX_DEVICES is not defined: build the kernel through CMake (see CMakeLists.txt)"
#endif
#ifndef SEELE_MAX_RESOURCES
#error "SEELE_MAX_RESOURCES is not defined: build the kernel through CMake (see CMakeLists.txt)"
#endif
constexpr inline size_t MAX_DEVICES = SEELE_MAX_DEVICES;
constexpr inline size_t MAX_RESOURCES = SEELE_MAX_RESOURCES;

// Probe state, owned by the driver core. `Unbound` is also the "no driver has
// claimed me yet" state. Every matched node is probed at most once.
enum class Status : uint8_t {
    Unbound,
    Bound,
    Failed,
};

// One device described by the firmware.
//
// The node itself is kept in platform/device.cc, not here: fdt::Node is an
// incomplete type in this header and the parser does not expose a node's byte
// offset. `m_node` is the index of that node handle, stable because devices are
// never removed for the kernel's lifetime. The tree itself is the file-scope
// `static fdt::Tree` in kmain.cc and outlives every device, so no per-device
// tree pointer is needed here.
class Device {
public:
    Device() noexcept = default;

    // The node name as written in the tree, e.g. "serial@10000000".
    [[nodiscard]] std::string_view name() const noexcept;

    // The first compatible string, e.g. "ns16550a".
    [[nodiscard]] std::string_view compatible() const noexcept;

    [[nodiscard]] bool is_compatible(std::string_view id) const noexcept;

    [[nodiscard]] std::span<const resource> resources() const noexcept;

    // The `index`-th resource of `type`, or nullptr when there is none.
    [[nodiscard]] const resource* find_resource(ResourceType type, size_t index = 0) const noexcept;

    // Driver-private attributes, decoded here so a driver never learns that a
    // device tree exists. `std::nullopt` when the property is absent or has the
    // wrong shape.
    [[nodiscard]] std::optional<uint32_t> u32_property(std::string_view name) const noexcept;
    [[nodiscard]] std::optional<std::string_view>
        string_property(std::string_view name) const noexcept;

    [[nodiscard]] Status status() const noexcept {
        return m_status;
    }

    void set_status(Status status) noexcept {
        m_status = status;
    }

    // The successfully probed object, not the raw allocation it was constructed
    // in. Subsystems borrow its interfaces; the object outlives the kernel.
    [[nodiscard]] Object* object() const noexcept {
        return m_object;
    }

    void set_object(Object* object) noexcept {
        m_object = object;
    }

private:
    friend void enumerate(const fdt::Tree& tree) noexcept;

    // Index into the bus's node-handle table, not a tree node offset.
    int m_node = -1;
    uint16_t m_first_resource = 0;
    uint16_t m_resource_count = 0;
    Status m_status = Status::Unbound;
    Object* m_object = nullptr;
};

// Every enumerated device, in depth-first tree order.
[[nodiscard]] std::span<Device> all() noexcept;

// The device with the given id, or nullptr when the id is out of range.
[[nodiscard]] Device* by_id(uint32_t id) noexcept;

// Walk the tree and fill the static device and resource arrays. Called once by
// kmain(), before any driver probes.
void enumerate(const fdt::Tree& tree) noexcept;

}  // namespace device
