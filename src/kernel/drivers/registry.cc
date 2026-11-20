// The driver core: match, probe order and private state.
//
// This file knows nothing about any concrete driver. It only sees the
// descriptor table through registered() (defined in drivers/table.cc), so the
// core stays independent of how drivers are registered.

#include "drivers/registry.h"

#include <cstddef>
#include <cstdint>
#include <new>
#include <span>

#include "console.h"
#include "panic.h"
#include "platform/device.h"

namespace driver {
namespace {

// Capacity of the probe-order array below. A build parameter
// (-DSEELE_MAX_DRIVERS): it bounds the driver table, not the device table, so
// it only has to cover the number of registered drivers, each of which costs
// one pointer.
#ifndef SEELE_MAX_DRIVERS
#error "SEELE_MAX_DRIVERS is not defined: build the kernel through CMake (see CMakeLists.txt)"
#endif
constexpr size_t MAX_DRIVERS = SEELE_MAX_DRIVERS;

// Backing store for driver-private state.
//
// This is a bump arena, not a general allocator: a probed driver lives for the
// rest of the kernel, so nothing is ever freed, there is no per-block header
// and there are no size classes. That is what makes the permanent, tiny state
// of a driver cheap -- one counter plus an array in .bss. Taking the same
// state from the kernel heap would be much more expensive: the heap maps a
// whole page on first use (12 KiB once the two page-table levels are counted)
// to hand out a single 40-byte object, while the arena's bytes are already in
// the image's .bss.
//
// The budget is a concrete number, and the two drivers in the table today set
// it: the PLIC keeps a 32-byte state (its handler table lives in trap.cc now),
// the UART 304 bytes (two base-class virtual tables, the register window, the
// programming parameters, its lock, the interrupt state, the panic and reader
// latches, the overrun counter and the 128-byte transmit plus 64-byte receive
// queue), and a machine needs one of each, so one boot uses 336 of the 1 KiB and
// leaves ~688 bytes. Growing a driver's state or the table means rechecking this
// number; exhaustion is a warning and a Failed device, not a panic.
constexpr size_t ARENA_SIZE = 1024;
constexpr size_t ARENA_ALIGN = 16;

alignas(ARENA_ALIGN) std::byte g_arena[ARENA_SIZE];
size_t g_arena_used = 0;

// Take `size` bytes out of the arena, or nullptr when it is full. The storage
// is already zeroed: cxx_init() clears .bss before the first probe, and a block
// is never handed out twice. The block is never returned either, because a
// probed driver lives for the rest of the kernel.
//
// probe() is called by the boot hart before the secondaries are released, so
// the bump counter needs no lock today; a multi-hart probe path would have to
// add one.
[[nodiscard]] void* arena_alloc(size_t size) noexcept {
    const size_t aligned = (g_arena_used + ARENA_ALIGN - 1) & ~(ARENA_ALIGN - 1);
    if(aligned + size > ARENA_SIZE) {
        return nullptr;
    }
    void* block = g_arena + aligned;
    g_arena_used = aligned + size;
    return block;
}

// Wire a bound device into the subsystem its declared type names. The type is
// part of the driver table, so the core needs no per-driver callback: for a
// console it knows the state block starts with the CharDevice the driver bound
// (registry.h), which is why one cast is all it takes.
void wire_device(Type type, void* priv) noexcept {
    switch(type) {
        case Type::Console: console::attach(*static_cast<CharDevice*>(priv)); return;
        case Type::Plain: return;
    }
}

void log_probe(const descriptor& descriptor, const device::Device& device) noexcept {
    // The console may not exist yet (a UART driver is what normally creates
    // it), and a log line must never be the thing that crashes early boot.
    if(!console::attached()) {
        return;
    }
    LOG_DEBUG("driver {}: {}", descriptor.name, device.name());
}

// Record a successful probe and wire the device into the subsystem its declared
// type names. Both the first pass and the Deferred retry end here, so "Bound"
// means "wired" on every path instead of only the one the author had in mind.
void bind_device(const descriptor& descriptor, device::Device& device, void* priv) noexcept {
    device.set_status(device::Status::Bound);
    if(priv == nullptr) {
        // A driver that declares a type has to have somewhere to keep the
        // interface; priv_size == 0 is a table bug, not a reason to skip wiring.
        SEELE_ASSERT(descriptor.type == Type::Plain,
                     "driver {}: declared {} but has no private state",
                     descriptor.name,
                     static_cast<int>(descriptor.type));
        return;
    }
    wire_device(descriptor.type, priv);
}

// Run one probe and record the result on the device.
void run_probe(device::Device& device, const descriptor& descriptor, void* priv) noexcept {
    const Status status =
        descriptor.probe != nullptr ? descriptor.probe(device, priv) : Status::Error;
    switch(status) {
        case Status::Ok: bind_device(descriptor, device, priv); break;
        case Status::Deferred: device.set_status(device::Status::Deferred); break;
        case Status::Error: device.set_status(device::Status::Failed); break;
    }
    log_probe(descriptor, device);
}

// Sort this level's descriptors by (priority descending, registration order).
// The list is tiny and rebuilt per level, so insertion sort keeps the state
// simple; the array is static to avoid a stack copy of the table on boot.
void order_level(InitLevel level, const descriptor** order, size_t& order_count) noexcept {
    order_count = 0;
    for(const descriptor& descriptor: registered()) {
        if(descriptor.level != level) {
            continue;
        }
        if(order_count == MAX_DRIVERS) {
            LOG_WARN("driver: more than {} descriptors in one level, ignoring {}",
                     MAX_DRIVERS,
                     descriptor.name);
            break;
        }
        // Only a strictly larger priority moves ahead, so equal priorities keep
        // registration order: "first registered wins" is what match() promises.
        size_t position = order_count;
        while(position > 0 && order[position - 1]->priority < descriptor.priority) {
            order[position] = order[position - 1];
            --position;
        }
        order[position] = &descriptor;
        ++order_count;
    }
}

}  // namespace

const descriptor* match(const device::Device& device) noexcept {
    const descriptor* best = nullptr;
    for(const descriptor& descriptor: registered()) {
        if(descriptor.match == nullptr) {
            continue;
        }

        bool matches = false;
        for(const char* const* id = descriptor.match; *id != nullptr; ++id) {
            if(device.is_compatible(*id)) {
                matches = true;
                break;
            }
        }
        if(!matches) {
            continue;
        }

        if(best == nullptr || descriptor.priority > best->priority) {
            best = &descriptor;
        }
    }
    return best;
}

void probe(InitLevel level, std::span<device::Device> devices) noexcept {
    const static descriptor* order[MAX_DRIVERS];
    size_t order_count = 0;
    order_level(level, order, order_count);

    for(size_t i = 0; i < order_count; ++i) {
        const descriptor& descriptor = *order[i];
        for(device::Device& device: devices) {
            if(device.status() != device::Status::Unbound) {
                continue;
            }
            // A device belongs to exactly one level: the one its highest
            // priority match lives in. Checking match() here (instead of the
            // level-local list alone) keeps a generic driver from stealing a
            // device that a more specific driver outranks.
            if(match(device) != &descriptor) {
                continue;
            }

            void* priv = nullptr;
            if(descriptor.priv_size != 0) {
                priv = arena_alloc(descriptor.priv_size);
                if(priv == nullptr) {
                    // No state means no probe. Mark the device failed instead of
                    // leaving it Unbound: a later level must not silently bind a
                    // different driver to a device whose resources are already
                    // half-consumed.
                    LOG_WARN("driver: private arena exhausted, skipping {}", descriptor.name);
                    device.set_status(device::Status::Failed);
                    continue;
                }
            }
            device.set_driver_data(priv);
            run_probe(device, descriptor, priv);
        }
    }

    // Deferred: the driver asked for a retry because something it needs was not
    // ready yet. One retry per level is the whole policy for now; a second
    // failure is terminal so the device does not stay Unbound forever.
    for(device::Device& device: devices) {
        if(device.status() != device::Status::Deferred) {
            continue;
        }
        const descriptor* descriptor = match(device);
        if(descriptor == nullptr || descriptor->level != level) {
            continue;
        }

        const Status status = descriptor->probe != nullptr
                                  ? descriptor->probe(device, device.driver_data())
                                  : Status::Error;
        if(status == Status::Ok) {
            bind_device(*descriptor, device, device.driver_data());
        } else {
            device.set_status(device::Status::Failed);
            LOG_WARN("driver: {} deferred and failed again, giving up", descriptor->name);
        }
        log_probe(*descriptor, device);
    }
}

void probe_all(std::span<device::Device> devices) noexcept {
    probe(InitLevel::Core, devices);
    probe(InitLevel::Device, devices);
    probe(InitLevel::Late, devices);
}

}  // namespace driver
