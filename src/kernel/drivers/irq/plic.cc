// SiFive PLIC driver (drivers/irq/plic.h).
//
// The register map is the one from the PLIC specification and the
// `riscv,plic0`/`sifive,plic-1.0.0` bindings:
//
//   priority       0x000000 + 4 * source          one 32-bit priority per source
//   pending        0x001000                       one bit per source, read-only
//   enable bitmap  0x002000 + 0x80 * context      1024 sources, 32 words
//   threshold      0x200000 + 0x1000 * context    word 0
//   claim/complete 0x200000 + 0x1000 * context    word 1
//
// Sources are numbered 1..riscv,ndev; bit 0 of the bitmap is reserved for
// source 0, which means "no interrupt" in a claim.

#include "drivers/irq/plic.h"

#include <cstdint>
#include <new>
#include <optional>

#include "console.h"
#include "trap.h"
#include "platform/device.h"
#include "platform/mmio.h"

namespace drivers::irq {
namespace {

constexpr uint64_t PRIORITY_BASE = 0x000000;
constexpr uint64_t PRIORITY_STRIDE = 4;

constexpr uint64_t ENABLE_BASE = 0x002000;
constexpr uint64_t ENABLE_CONTEXT_STRIDE = 0x80;

constexpr uint64_t CONTEXT_BASE = 0x200000;
constexpr uint64_t CONTEXT_STRIDE = 0x1000;

// The enable window is 0x80 bytes, i.e. 32 words of 32 bits: the PLIC spec
// caps the source count at 1024 for exactly that reason.
constexpr uint32_t MAX_SOURCES = 1024;

// The context this driver programs. QEMU virt's `interrupts-extended` lists
// hart 0 as (M-mode context 0, S-mode context 1), then hart 1 as (2, 3) and so
// on; the boot hart probes, so its S-mode context is 1. See plic.h for why the
// other contexts are left alone.
constexpr uint32_t BOOT_HART_S_CONTEXT = 1;

// Enough of the window to reach the boot hart's threshold and claim registers.
constexpr uint64_t MIN_REGION_SIZE = CONTEXT_BASE + (BOOT_HART_S_CONTEXT + 1) * CONTEXT_STRIDE;

}  // namespace

Plic::Plic(mmio::Region region, uint32_t sources, uint32_t context) noexcept :
    m_region(region), m_sources(sources), m_context(context) {}

void Plic::init() noexcept {
    // A source left enabled by firmware would deliver an interrupt whose
    // handler this kernel has not registered, and the claim loop would only
    // complete it without servicing it. Start from a context with nothing
    // enabled.
    const uint32_t words = (m_sources + 31) / 32;
    for(uint32_t word = 0; word < words; ++word) {
        m_region.write<uint32_t>(ENABLE_BASE + m_context * ENABLE_CONTEXT_STRIDE + word * 4, 0);
    }

    // Threshold 0 accepts every priority above 0; priorities are programmed per
    // source by enable(), and a priority of 0 masks a source.
    m_region.write<uint32_t>(threshold_offset(), 0);
    m_region.fence();
}

bool Plic::enable(uint32_t source, uint32_t priority) noexcept {
    if(source == 0 || source > m_sources) {
        return false;
    }

    m_region.write<uint32_t>(PRIORITY_BASE + PRIORITY_STRIDE * source, priority);
    // Priority 0 masks the source by definition; clear a stale enable bit in
    // that case instead of leaving the handler registered but deliverable.
    set_enabled(source, priority != 0);
    m_region.fence();
    return true;
}

void Plic::handle() noexcept {
    // Claiming is level-based: a source whose handler did not clear its
    // condition is claimed again as soon as its interrupt is completed. The
    // bound is what keeps such a source from pinning this hart inside the trap
    // handler; anything it leaves pending waits for the next external interrupt
    // (which the current one-shot trap path does not deliver, see trap.cc).
    // In practice one claim is the common case.
    for(uint32_t dispatched = 0; dispatched < m_sources; ++dispatched) {
        const uint32_t source = claim();
        if(source == 0) {
            return;
        }

        trap::dispatch_external_handler(source);

        // Complete even without a handler: a claimed source that is never
        // completed would stay pending (and unclaimable) forever.
        complete(source);
    }
}

uint64_t Plic::enable_offset(uint32_t source) const noexcept {
    return ENABLE_BASE + m_context * ENABLE_CONTEXT_STRIDE + (source / 32) * 4;
}

uint64_t Plic::threshold_offset() const noexcept {
    return CONTEXT_BASE + m_context * CONTEXT_STRIDE;
}

uint64_t Plic::claim_offset() const noexcept {
    return CONTEXT_BASE + m_context * CONTEXT_STRIDE + 4;
}

void Plic::set_enabled(uint32_t source, bool enabled) noexcept {
    const uint64_t offset = enable_offset(source);
    const uint32_t bit = 1u << (source % 32);
    const uint32_t word = m_region.read<uint32_t>(offset);
    m_region.write<uint32_t>(offset, enabled ? (word | bit) : (word & ~bit));
}

uint32_t Plic::claim() noexcept {
    // The read must happen before the handler touches its device: a handler
    // that clears the condition before the claim has been read could otherwise
    // lose the interrupt. The fence orders the claim read before the handler's
    // stores.
    const uint32_t source = m_region.read<uint32_t>(claim_offset());
    m_region.fence();
    return source;
}

void Plic::complete(uint32_t source) noexcept {
    // And the other direction: every store the handler made has to be visible
    // before the gateway is told the source is done, or the device can
    // re-assert while its state change is still in flight.
    m_region.fence();
    m_region.write<uint32_t>(claim_offset(), source);
}

device::Object* probe(device::Device& device, void* priv) noexcept {
    if(priv == nullptr) {
        return nullptr;
    }

    const auto region = mmio::map(device);
    if(!region || region->size() < MIN_REGION_SIZE) {
        return nullptr;
    }

    // `riscv,ndev` is the source count the firmware says the controller
    // implements; without it the enable bitmap has no bound and an out-of-range
    // request could not be rejected.
    const std::optional<uint32_t> sources = device.u32_property("riscv,ndev");
    if(!sources.has_value() || *sources == 0 || *sources > MAX_SOURCES) {
        return nullptr;
    }

    // The probe owns the placement new: the driver core only hands over a
    // zeroed, aligned block, and the controller lives there for the rest of the
    // kernel.
    auto* self = ::new (priv) Plic{*region, *sources, BOOT_HART_S_CONTEXT};
    self->init();

    // The early console is attached by now, so the details are worth a line:
    // the source count proves the `riscv,ndev` property was decoded and the
    // context names which hart will service external interrupts.
    LOG_DEBUG("plic: {} sources on S-mode context {}", self->source_count(), self->context());

    // The core discovers ExternalController on this object and installs it
    // before any Device-level driver registers an interrupt source.
    return self;
}

}  // namespace drivers::irq
