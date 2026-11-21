#pragma once

// The driver core: match devices against drivers, probe in level order, and
// hand each driver a private state block.
//
// A descriptor is plain constant data (a name, a compatibility list, a level,
// a priority and a function pointer), so the registry is a compile-time table
// in drivers/table.cc. The core only sees it
// through registered(), so it never references a concrete driver; adding a
// driver does not touch this file.

#include <cstddef>
#include <cstdint>
#include <span>

#include "platform/device.h"

namespace driver {

// Probe order. Levels are a hard boundary: a lower level runs to completion
// before the next one starts. Core is for the allocator and interrupt
// controller; Device for ordinary peripherals; Late for drivers that need
// interrupts or a work queue.
//
// There is deliberately no Early level. The console and the other things that
// must work before paging are firmware-shaped and live in src/kernel/early:
// they cannot depend on device enumeration, the private-state arena or the
// console lock.
enum class InitLevel : uint8_t {
    Core,
    Device,
    Late,
};

// Alignment guaranteed by the private-state arena. Drivers that construct
// their object there should assert that its alignment fits this contract.
constexpr inline size_t PRIVATE_STATE_ALIGN = 16;

// `priv` is the driver's zero-initialised state block (or nullptr when
// priv_size is zero), aligned to 16 bytes -- the fundamental alignment. A
// driver whose state is over-aligned must allocate that storage itself and
// keep only a pointer here. Probe constructs and initialises an object, then
// returns it on success, or nullptr on failure. The object and its interfaces
// must live for the rest of the kernel; the core discovers and attaches them.
using Probe = device::Object* (*)(device::Device & device, void* priv) noexcept;

struct descriptor {
    const char* name;
    const char* const* match;  // nullptr-terminated list of compatible strings
    InitLevel level;
    int priority;        // larger wins, so a specific driver can outrank a generic one
    uint32_t priv_size;  // 0 means no private state; the core passes nullptr
    Probe probe;
};

// Every driver in the image, in registration order. Implemented by
// drivers/table.cc; the driver core only iterates it.
[[nodiscard]] std::span<const descriptor> registered() noexcept;

// The descriptor that should drive `device`: the match with the highest
// priority, earliest registration first on a tie. nullptr when nothing matches.
[[nodiscard]] const descriptor* match(const device::Device& device) noexcept;

// Probe every unbound device whose best match belongs to `level`, in
// (priority descending, registration order) order. Each device is probed once.
void probe(InitLevel level, std::span<device::Device> devices) noexcept;

// probe() at each level in order: Core, Device, Late.
void probe_all(std::span<device::Device> devices) noexcept;

}  // namespace driver

// One line per driver in drivers/table.cc: expands to a braced initialiser for
// that file's `constinit const descriptor kDrivers[]`, so it must appear inside
// the array's initialiser list.
#define SEELE_DRIVER(tag, match, level, priority, priv_size, probe)                                \
    ::driver::descriptor {                                                                         \
        #tag, match, level, priority, priv_size, probe                                             \
    }
