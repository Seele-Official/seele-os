// SiFive test finisher driver (drivers/power/sifive_test.h).
//
// Register map (`sifive,test1` / `sifive,test0` on QEMU `virt` at 0x100000):
//   finisher  0x0000  32-bit command register:
//                       0x5555               -> power off with exit code 0
//                       (code << 16) | 0x3333 -> power off with non-zero `code`

#include "drivers/power/sifive_test.h"

#include <cstdint>
#include <new>

#include "device_object.h"
#include "power.h"
#include "drivers/registry.h"
#include "platform/device.h"
#include "platform/mmio.h"

namespace drivers::power {
namespace {

constexpr const char* MATCH[] = {"sifive,test1", "sifive,test0", nullptr};

constexpr uint64_t FINISHER_OFFSET = 0x0;
constexpr uint64_t MIN_REGION_SIZE = sizeof(uint32_t);

constexpr uint32_t FINISHER_FAIL = 0x3333u;
constexpr uint32_t FINISHER_PASS = 0x5555u;

class SifiveTest final : public device::Implements<PowerDevice> {
public:
    explicit SifiveTest(mmio::Region region) noexcept : m_region(region) {}

    SifiveTest(const SifiveTest&) = delete;
    SifiveTest& operator= (const SifiveTest&) = delete;

    void poweroff(uint16_t code) noexcept override {
        const uint32_t command =
            code == 0 ? FINISHER_PASS : ((static_cast<uint32_t>(code) << 16) | FINISHER_FAIL);
        m_region.fence();
        m_region.write<uint32_t>(FINISHER_OFFSET, command);
        m_region.fence();
    }

private:
    mmio::Region m_region;
};

static_assert(alignof(SifiveTest) <= driver::PRIVATE_STATE_ALIGN);

device::Object* probe(device::Device& device, void* priv) noexcept {
    if(priv == nullptr) {
        return nullptr;
    }

    const auto region = mmio::map(device);
    if(!region || region->size() < MIN_REGION_SIZE) {
        return nullptr;
    }

    return ::new (priv) SifiveTest{*region};
}

}  // namespace

constinit const driver::descriptor SIFIVE_TEST_DRIVER{
    .name = "sifive-test",
    .match = MATCH,
    .level = driver::InitLevel::Core,
    .priority = 0,
    .priv_size = sizeof(SifiveTest),
    .probe = probe,
};

}  // namespace drivers::power
