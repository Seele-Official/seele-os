// The early console (early/firmware.h).
//
// This file is the kernel's firmware phase: it runs before the driver core, the
// page allocator and the console lock, so it uses none of them, and it never
// calls LOG_* or SEELE_* -- a panic here would try to print through the console
// that is not attached yet.
//
// It does reuse the Ns16550 driver class: the register programming is identical
// to what the Device-level driver does, and duplicating it would be one more
// copy to keep in sync. What it does not reuse is the driver model around it:
// no device enumeration, no private-state arena, no probe descriptor.

#include "early/firmware.h"

#include <cstddef>
#include <cstdint>
#include <new>
#include <string_view>

#include "console.h"
#include "drivers/uart/ns16550.h"
#include "mm/addr.h"
#include "mm/vm.h"
#include "platform/mmio.h"

namespace early {
namespace {

// How deep the firmware walk descends. Shared with the device enumerator's own
// walk (platform/device.cc) through one build parameter: both read the same
// tree, and a cap is what keeps a malformed (cyclic-looking) structure block
// from recursing forever.
#ifndef SEELE_FDT_MAX_DEPTH
#error "SEELE_FDT_MAX_DEPTH is not defined: build the kernel through CMake (see CMakeLists.txt)"
#endif
constexpr uint32_t MAX_DEPTH = SEELE_FDT_MAX_DEPTH;

// Highest register number the Ns16550 class addresses (SCR).
constexpr uint32_t HIGHEST_REGISTER = 7;

// The driver object lives for the whole kernel, so it gets static storage
// instead of the driver core's arena. Placement new is used because Ns16550
// has no default constructor (it takes its mmio::Region).
alignas(drivers::uart::Ns16550) std::byte g_storage[sizeof(drivers::uart::Ns16550)];

[[nodiscard]] bool is_console_compatible(const fdt::Node& node) noexcept {
    return node.is_compatible("ns16550a") || node.is_compatible("ns16550");
}

[[nodiscard]] fdt::Node find_console(fdt::Node node, uint32_t depth) noexcept {
    if(!node || depth > MAX_DEPTH) {
        return {};
    }
    if(node.enabled() && is_console_compatible(node)) {
        return node;
    }

    for(fdt::Node child = node.first_child(); child; child = child.next_sibling()) {
        if(fdt::Node found = find_console(child, depth + 1)) {
            return found;
        }
    }
    return {};
}

// `/chosen/stdout-path` wins when it is a path. Aliases ("serial0") are not
// resolved here: the early phase stays a one-device parser, not a resolver.
[[nodiscard]] fdt::Node console_from_chosen(const fdt::Tree& tree) noexcept {
    const fdt::Node chosen = tree.find("/chosen");
    if(!chosen) {
        return {};
    }

    const auto path = chosen.property("stdout-path").string();
    if(!path) {
        return {};
    }

    // "serial0:115200n8" -- the part after ':' is options, not path text.
    std::string_view value = *path;
    const size_t colon = value.find(':');
    if(colon != std::string_view::npos) {
        value = std::string_view{value.data(), colon};
    }
    if(value.empty() || value.front() != '/') {
        return {};
    }
    return tree.find(value);
}

// Reject anything the driver would read or write outside its window. The checks
// mirror what the driver's probe does with the same properties, except that a
// bad value is a refusal here instead of a clamp: the early phase has nothing
// to report a correction through.
[[nodiscard]] bool attach_uart(const fdt::Tree& tree, fdt::Node node) noexcept {
    fdt::RegIterator registers = tree.reg(node);
    Paddr base{};
    uint64_t size = 0;
    if(!registers.next(base, size)) {
        return false;
    }

    uint32_t shift = node.property("reg-shift").u32().value_or(0);
    uint32_t width = node.property("reg-io-width").u32().value_or(1);
    if(shift > 3) {
        shift = 0;
    }
    if(width != 1 && width != 4) {
        width = 1;
    }

    if(size < (uint64_t{HIGHEST_REGISTER} << shift) + width) {
        return false;
    }
    if((to_uint64(base) & (width - 1)) != 0) {
        return false;  // a misaligned MMIO access would trap
    }

    auto* uart = ::new (static_cast<void*>(g_storage)) drivers::uart::Ns16550{
        mmio::Region{vm::k::P2V(base), size}
    };
    uart->init(node.property("clock-frequency").u32().value_or(0), shift, width);

    // One device for output and input. It is polled here (no interrupt is
    // enabled this early), so console::read() would wait on the hart; the shell
    // only starts after the Device-level probe has replaced it.
    console::attach_boot(*uart);
    return true;
}

}  // namespace

bool attach_console(const fdt::Tree& tree) noexcept {
    fdt::Node node = console_from_chosen(tree);
    if(!node) {
        node = find_console(tree.root(), 0);
    }
    if(!node) {
        return false;
    }
    return attach_uart(tree, node);
}

}  // namespace early
