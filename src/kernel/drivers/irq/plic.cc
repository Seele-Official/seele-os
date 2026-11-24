// SiFive PLIC driver (drivers/irq/plic.h).
//
// Register map (`riscv,plic0` / `sifive,plic-1.0.0`):
//   priority       0x000000 + 4 * source          one 32-bit priority per source
//   pending        0x001000                       one bit per source, read-only
//   enable bitmap  0x002000 + 0x80 * context      1024 sources, 32 words
//   threshold      0x200000 + 0x1000 * context    word 0
//   claim/complete 0x200000 + 0x1000 * context    word 1
//
// Sources are numbered 1..riscv,ndev; source 0 means "no interrupt" in a claim.
// Currently programs only the boot hart's S-mode context (QEMU virt context 1).

#include "drivers/irq/plic.h"

#include <cstdint>
#include <new>
#include <optional>

#include "console.h"
#include "device_object.h"
#include "trap.h"
#include "drivers/registry.h"
#include "platform/device.h"
#include "platform/mmio.h"

namespace drivers::irq {
namespace {

constexpr const char* MATCH[] = {"sifive,plic-1.0.0", "riscv,plic0", nullptr};

constexpr uint64_t PRIORITY_BASE = 0x000000;
constexpr uint64_t PRIORITY_STRIDE = 4;

constexpr uint64_t ENABLE_BASE = 0x002000;
constexpr uint64_t ENABLE_CONTEXT_STRIDE = 0x80;

constexpr uint64_t CONTEXT_BASE = 0x200000;
constexpr uint64_t CONTEXT_STRIDE = 0x1000;

// The enable window is 0x80 bytes (32 words of 32 bits), capping sources at 1024.
constexpr uint32_t MAX_SOURCES = 1024;

// QEMU virt lists hart 0 as (M-mode context 0, S-mode context 1).
constexpr uint32_t BOOT_HART_S_CONTEXT = 1;

// Enough of the window to reach the boot hart's threshold and claim registers.
constexpr uint64_t MIN_REGION_SIZE = CONTEXT_BASE + (BOOT_HART_S_CONTEXT + 1) * CONTEXT_STRIDE;

class Plic final : public device::Implements<trap::ExternalController> {
public:
    Plic(mmio::Region region, uint32_t sources, uint32_t context) noexcept :
        m_region(region), m_sources(sources), m_context(context) {}

    Plic(const Plic&) = delete;
    Plic& operator= (const Plic&) = delete;

    void init() noexcept {
        // Start from a context with nothing enabled so firmware leftovers cannot
        // fire unhandled interrupts.
        const uint32_t words = (m_sources + 31) / 32;
        for(uint32_t word = 0; word < words; ++word) {
            m_region.write<uint32_t>(ENABLE_BASE + m_context * ENABLE_CONTEXT_STRIDE + word * 4, 0);
        }

        // Threshold 0 accepts every priority above 0; priority 0 masks a source.
        m_region.write<uint32_t>(threshold_offset(), 0);
        m_region.fence();
    }

    [[nodiscard]] bool enable(uint32_t source, uint32_t priority) noexcept override {
        if(source == 0 || source > m_sources) {
            return false;
        }

        m_region.write<uint32_t>(PRIORITY_BASE + PRIORITY_STRIDE * source, priority);
        set_enabled(source, priority != 0);
        m_region.fence();
        return true;
    }

    void handle() noexcept override {
        // Bound claims per trap so a stuck level-triggered source cannot pin the hart.
        for(uint32_t dispatched = 0; dispatched < m_sources; ++dispatched) {
            const uint32_t source = claim();
            if(source == 0) {
                return;
            }

            trap::dispatch_external_handler(source);

            // Complete even without a handler so the gateway does not wedge.
            complete(source);
        }
    }

    [[nodiscard]] uint32_t source_count() const noexcept {
        return m_sources;
    }

    [[nodiscard]] uint32_t context() const noexcept {
        return m_context;
    }

private:
    [[nodiscard]] uint64_t enable_offset(uint32_t source) const noexcept {
        return ENABLE_BASE + m_context * ENABLE_CONTEXT_STRIDE + (source / 32) * 4;
    }

    [[nodiscard]] uint64_t threshold_offset() const noexcept {
        return CONTEXT_BASE + m_context * CONTEXT_STRIDE;
    }

    [[nodiscard]] uint64_t claim_offset() const noexcept {
        return CONTEXT_BASE + m_context * CONTEXT_STRIDE + 4;
    }

    void set_enabled(uint32_t source, bool enabled) noexcept {
        const uint64_t offset = enable_offset(source);
        const uint32_t bit = 1u << (source % 32);
        const uint32_t word = m_region.read<uint32_t>(offset);
        m_region.write<uint32_t>(offset, enabled ? (word | bit) : (word & ~bit));
    }

    [[nodiscard]] uint32_t claim() noexcept {
        const uint32_t source = m_region.read<uint32_t>(claim_offset());
        m_region.fence();
        return source;
    }

    void complete(uint32_t source) noexcept {
        m_region.fence();
        m_region.write<uint32_t>(claim_offset(), source);
    }

    mmio::Region m_region;
    uint32_t m_sources;
    uint32_t m_context;
};

static_assert(alignof(Plic) <= driver::PRIVATE_STATE_ALIGN);

device::Object* probe(device::Device& device, void* priv) noexcept {
    if(priv == nullptr) {
        return nullptr;
    }

    const auto region = mmio::map(device);
    if(!region || region->size() < MIN_REGION_SIZE) {
        return nullptr;
    }

    const std::optional<uint32_t> sources = device.u32_property("riscv,ndev");
    if(!sources.has_value() || *sources == 0 || *sources > MAX_SOURCES) {
        return nullptr;
    }

    auto* self = ::new (priv) Plic{*region, *sources, BOOT_HART_S_CONTEXT};
    self->init();

    LOG_DEBUG("plic: {} sources on S-mode context {}", self->source_count(), self->context());
    return self;
}

}  // namespace

constinit const driver::descriptor PLIC_DRIVER{
    .name = "plic",
    .match = MATCH,
    .level = driver::InitLevel::Core,
    .priority = 0,
    .priv_size = sizeof(Plic),
    .probe = probe,
};

}  // namespace drivers::irq
