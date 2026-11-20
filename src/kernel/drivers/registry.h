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

enum class Status : uint8_t {
    Ok,
    Deferred,
    Error,
};

// `priv` is the driver's zero-initialised state block (or nullptr when
// priv_size is zero), aligned to 16 bytes -- the fundamental alignment. A
// driver whose state is over-aligned must allocate that storage itself and
// keep only a pointer here. Returning Deferred asks the core to retry after the
// rest of the level has run.
using Probe = Status (*)(device::Device& device, void* priv) noexcept;

// What kind of device a driver binds, declared in the driver table.
//
// This is the device's own statement about itself: the core wires a bound device
// from the declaration alone, without asking the driver through a callback. A
// driver that names a type must lay its state block out accordingly -- the
// type's interface has to be its first base, so `void* priv` already points at
// the interface -- and the driver asserts that layout when it binds.
enum class Type : uint8_t {
    // No subsystem to join: probe() does everything the device needs.
    Plain,
    // A console: a CharDevice. After a successful probe the core hands the
    // state block to console::attach() as the console's sink and input source.
    Console,
};

struct descriptor {
    const char* name;
    const char* const* match;  // nullptr-terminated list of compatible strings
    InitLevel level;
    int priority;        // larger wins, so a specific driver can outrank a generic one
    uint32_t priv_size;  // 0 means no private state; the core passes nullptr
    Probe probe;
    // What the bound device is; the core wires it by this declaration. Plain is
    // the default because most drivers join no subsystem.
    Type type = Type::Plain;
};

// Every driver in the image, in registration order. Implemented by
// drivers/table.cc; the driver core only iterates it.
[[nodiscard]] std::span<const descriptor> registered() noexcept;

// The descriptor that should drive `device`: the match with the highest
// priority, earliest registration first on a tie. nullptr when nothing matches.
[[nodiscard]] const descriptor* match(const device::Device& device) noexcept;

// Probe every unbound device whose best match belongs to `level`, in
// (priority descending, registration order) order. Deferred devices are retried
// once when the level ends.
void probe(InitLevel level, std::span<device::Device> devices) noexcept;

// probe() at each level in order: Core, Device, Late.
void probe_all(std::span<device::Device> devices) noexcept;

}  // namespace driver

// One line per driver in drivers/table.cc: expands to a braced initialiser for
// that file's `constinit const descriptor kDrivers[]`, so it must appear inside
// the array's initialiser list.
#define SEELE_DRIVER(tag, match, level, priority, priv_size, probe, type)                          \
    ::driver::descriptor {                                                                         \
        #tag, match, level, priority, priv_size, probe, type                                       \
    }
