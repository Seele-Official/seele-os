#pragma once

// Platform-level interrupt controller: the SiFive PLIC binding.
//
// The PLIC is the piece between a device's interrupt line and the hart: a
// source that has been enabled and whose priority beats the context's threshold
// stays pending until software claims it, services it and completes it. This
// driver owns that protocol and nothing else: the source -> handler table lives
// one layer up, in trap.h, so a device driver asks the trap layer for an
// interrupt number and never includes this header.
//
// The split is the trap::ExternalController contract in trap.h:
//
//   - the core discovers and installs the controller returned by probe; the trap path
//     calls handle(), which claims a source, asks
//     trap::dispatch_external_handler() to run the registered handler, and
//     completes the source.
//   - registering a handler (trap::register_external_handler) calls back into
//     enable(source, priority), which programs the priority and sets the
//     context's enable bit.
//
// One context is programmed: the boot hart's S-mode context. PLIC context
// numbering is defined by the firmware's `interrupts-extended` list, and QEMU
// virt lists each hart twice (M-mode, then S-mode), so hart 0's S-mode context
// is 1. Probe runs on hart 0, the boot hart, and every external interrupt is
// serviced there. The other harts' S-mode contexts stay disabled: enabling them
// needs a hartid -> context map, which the kernel does not record yet (hart.h),
// and an external interrupt on a hart that is not the console's drainer would
// only add a second path to the same device. `idle()` still enables sie.SEIE on
// every hart; with no enabled PLIC context that simply produces no interrupt.

#include <cstdint>

#include "trap.h"
#include "drivers/registry.h"
#include "platform/device.h"
#include "platform/mmio.h"

namespace drivers::irq {

// Matched against the device's compatible list, most specific first. Both
// strings appear together on QEMU virt's PLIC; `riscv,plic0` is the older
// spelling of the same binding.
constexpr const inline char* kMatch[] = {"sifive,plic-1.0.0", "riscv,plic0", nullptr};

class Plic final : public device::Implements<trap::ExternalController> {
public:
    // `sources` is the device's `riscv,ndev`: sources are numbered 1..sources.
    // `context` is the PLIC context index this driver programs (see the file
    // comment; the probe passes the boot hart's S-mode context).
    Plic(mmio::Region region, uint32_t sources, uint32_t context) noexcept;

    Plic(const Plic&) = delete;
    Plic& operator= (const Plic&) = delete;

    // Clear every enable bit of this context and program its threshold to
    // "accept any priority". Called once by the probe, before any source is
    // enabled.
    void init() noexcept;

    // Controller half of trap::ExternalController::enable: program `source`'s
    // priority and enable it on this context. `priority` 0 registers the source
    // masked (the handler stays in trap's table but cannot fire). False for an
    // out-of-range source; the caller (trap::register_external_handler) then
    // rolls the registration back.
    [[nodiscard]] bool enable(uint32_t source, uint32_t priority) noexcept override;

    // Controller half of trap::ExternalController::handle: claim pending
    // sources, dispatch each through the trap table, and complete it. Bounded by
    // the source count, so a source whose handler does not clear its condition
    // cannot pin the hart inside the trap handler.
    void handle() noexcept override;

    [[nodiscard]] uint32_t source_count() const noexcept {
        return m_sources;
    }

    [[nodiscard]] uint32_t context() const noexcept {
        return m_context;
    }

private:
    [[nodiscard]] uint64_t enable_offset(uint32_t source) const noexcept;
    [[nodiscard]] uint64_t threshold_offset() const noexcept;
    [[nodiscard]] uint64_t claim_offset() const noexcept;

    void set_enabled(uint32_t source, bool enabled) noexcept;
    [[nodiscard]] uint32_t claim() noexcept;
    void complete(uint32_t source) noexcept;

    mmio::Region m_region;
    uint32_t m_sources;
    uint32_t m_context;
};

// Private state the driver core reserves for this driver: exactly the object,
// as with ns16550. The object lives for the rest of the kernel, and it is small
// because the handler table it used to own moved to trap.cc.
constexpr inline uint32_t kPrivSize = sizeof(Plic);
static_assert(alignof(Plic) <= driver::PRIVATE_STATE_ALIGN);

device::Object* probe(device::Device& device, void* priv) noexcept;

}  // namespace drivers::irq
