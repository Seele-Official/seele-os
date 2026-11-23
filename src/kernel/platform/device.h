#pragma once

// The device model: firmware-described devices and their typed resources.
//
// `platform/device.cc` translates FDT nodes into `device::Device` with typed
// MMIO windows (`phys_range`) and interrupt lines (`irq_line`), so drivers
// never include `fdt/fdt.h` or decode raw device-tree cells.
//
// Storage is entirely static: an array of devices plus flat typed pools for
// MMIO ranges and IRQ lines. A device id is its index in `all()`.

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

struct irq_line {
    uint32_t controller;  // device id of the interrupt controller, or UINT32_MAX if unknown
    uint32_t number;      // interrupt number in that controller
    uint32_t trigger;     // 1=rising 2=falling 3=level-high 4=level-low, 0=unknown
};

// Static capacity bounds (-DSEELE_MAX_DEVICES, -DSEELE_MAX_RESOURCES).
// Overflow is truncated with a warning rather than panicking.
#ifndef SEELE_MAX_DEVICES
#error "SEELE_MAX_DEVICES is not defined: build the kernel through CMake (see CMakeLists.txt)"
#endif
#ifndef SEELE_MAX_RESOURCES
#error "SEELE_MAX_RESOURCES is not defined: build the kernel through CMake (see CMakeLists.txt)"
#endif
constexpr inline size_t MAX_DEVICES = SEELE_MAX_DEVICES;
constexpr inline size_t MAX_RESOURCES = SEELE_MAX_RESOURCES;

// Probe state, owned by the driver core. Every matched device is probed at most
// once.
enum class Status : uint8_t {
    Unbound,
    Bound,
    Failed,
};

// One device discovered from firmware.
//
// Stores only its structure-block byte offset (`m_fdt_offset`) into the
// kernel-lifetime `fdt::Tree` owned by `platform/platform.cc`, keeping
// `fdt/fdt.h` completely out of driver headers.
class Device {
public:
    Device() noexcept = default;

    // Node name as written in firmware, e.g. "serial@10000000".
    [[nodiscard]] std::string_view name() const noexcept;

    // First compatible string, e.g. "ns16550a".
    [[nodiscard]] std::string_view compatible() const noexcept;

    [[nodiscard]] bool is_compatible(std::string_view id) const noexcept;

    // Typed resource slices (O(1) access).
    [[nodiscard]] std::span<const phys_range> mmios() const noexcept;
    [[nodiscard]] std::span<const irq_line> irqs() const noexcept;
    [[nodiscard]] std::optional<phys_range> mmio(size_t index = 0) const noexcept;
    [[nodiscard]] std::optional<irq_line> irq(size_t index = 0) const noexcept;

    // Driver-specific firmware properties. Returns std::nullopt when absent or
    // malformed.
    [[nodiscard]] std::optional<uint32_t> u32_property(std::string_view name) const noexcept;
    [[nodiscard]] std::optional<std::string_view>
        string_property(std::string_view name) const noexcept;

    [[nodiscard]] Status status() const noexcept {
        return m_status;
    }

    void set_status(Status status) noexcept {
        m_status = status;
    }

    // Borrowed pointer to the probed driver object; outlives the kernel.
    [[nodiscard]] Object* object() const noexcept {
        return m_object;
    }

    void set_object(Object* object) noexcept {
        m_object = object;
    }

private:
    friend void enumerate(const fdt::Tree& tree) noexcept;

    int m_fdt_offset = -1;
    uint16_t m_mmio_begin = 0;
    uint16_t m_mmio_count = 0;
    uint16_t m_irq_begin = 0;
    uint16_t m_irq_count = 0;
    Status m_status = Status::Unbound;
    Object* m_object = nullptr;
};

// Every enumerated device, in depth-first tree order.
[[nodiscard]] std::span<Device> all() noexcept;

// Device with the given id, or nullptr when out of range.
[[nodiscard]] Device* by_id(uint32_t id) noexcept;

// Walk `tree` and populate the static device and resource pools. `tree` must
// remain alive for the entire kernel lifetime.
void enumerate(const fdt::Tree& tree) noexcept;

}  // namespace device
