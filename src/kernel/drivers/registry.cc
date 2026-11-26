// The driver core: match, probe order and private state.
//
// This file knows nothing about any concrete driver. It only sees the
// descriptor table through registered() (defined in drivers/table.cc), so the
// core stays independent of how drivers are registered.

#include "drivers/registry.h"

#include <cstddef>
#include <cstdint>
#include <span>

#include "char_device.h"
#include "console.h"
#include "console_sink.h"
#include "power.h"
#include "trap.h"
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
// The current PLIC and UART objects fit comfortably in 1 KiB, including their
// interface subobjects and alignment padding. Growing a driver's state or the
// table means rechecking this budget; exhaustion is a warning and a Failed
// device, not a panic.
constexpr size_t ARENA_SIZE = 1024;
constexpr size_t ARENA_ALIGN = PRIVATE_STATE_ALIGN;

alignas(ARENA_ALIGN) std::byte g_arena[ARENA_SIZE];
size_t g_arena_used = 0;

// Take `size` bytes out of the arena, or nullptr when it is full. The storage
// is already zeroed: kmain() clears .bss before the first probe, and a block
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

// Query each supported subsystem independently: one object may expose more
// than one interface. The returned pointers have already been adjusted by C++
// base conversions, not reconstructed from the raw state block's address.
void wire_device(device::Object& object) noexcept {
    if(auto* controller = device::dyn_cast<trap::ExternalController>(&object)) {
        trap::set_external_controller(*controller);
    }
    if(auto* sink = device::dyn_cast<ConsoleSink>(&object)) {
        console::attach(*sink);
    }
    if(auto* chars = device::dyn_cast<CharDevice>(&object)) {
        char_device::attach(*chars);
    }
    if(auto* pwr = device::dyn_cast<PowerDevice>(&object)) {
        power::attach(*pwr);
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

// Probe once. Only successful, fully wired objects are published on the node;
// failed probes leave it without an object and never fall back to another driver.
void run_probe(device::Device& device, const descriptor& descriptor, void* priv) noexcept {
    device::Object* object = descriptor.probe != nullptr ? descriptor.probe(device, priv) : nullptr;
    if(object != nullptr) {
        wire_device(*object);
        device.set_object(object);
        device.set_status(device::Status::Bound);
    } else {
        device.set_status(device::Status::Failed);
    }
    log_probe(descriptor, device);
}

// Sort this level's descriptors by (priority descending, registration order).
// The list is tiny and rebuilt per level, so insertion sort keeps it simple.
void order_level(InitLevel level, const descriptor** order, size_t& order_count) noexcept {
    order_count = 0;
    for(const descriptor* entry: registered()) {
        if(entry == nullptr || entry->level != level) {
            continue;
        }
        const descriptor& descriptor = *entry;
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
    for(const descriptor* entry: registered()) {
        if(entry == nullptr || entry->match == nullptr) {
            continue;
        }
        const descriptor& descriptor = *entry;

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
    const descriptor* order[MAX_DRIVERS];
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
            run_probe(device, descriptor, priv);
        }
    }
}

void probe_all(std::span<device::Device> devices) noexcept {
    probe(InitLevel::Core, devices);
    probe(InitLevel::Device, devices);
    probe(InitLevel::Late, devices);
}

}  // namespace driver
