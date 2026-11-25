#include <cstddef>
#include <cstdint>
#include <string.h>

#include "char_device.h"
#include "console.h"
#include "device_object.h"
#include "power.h"
#include "trap.h"
#include "platform/device.h"
#include "platform/platform.h"
#include "test/ktest.h"

namespace {

KTEST_CASE(platform, boot_topology_and_drivers) {
    const platform::Info& info = platform::info();
    KTEST_EXPECT(!info.memory().usable().empty());
    KTEST_EXPECT(!info.memory().reserved().empty());
    KTEST_EXPECT(!info.harts().empty());
    KTEST_EXPECT(info.timebase_frequency() > 0);
    KTEST_EXPECT(console::attached());
    KTEST_EXPECT(power::can_poweroff());

    bool found_irq_controller = false;
    bool found_char_device = false;
    bool found_power_device = false;

    for(const device::Device& dev: device::all()) {
        if(dev.status() != device::Status::Bound || dev.object() == nullptr) {
            continue;
        }
        if(device::dyn_cast<trap::ExternalController>(dev.object()) != nullptr) {
            found_irq_controller = true;
            KTEST_EXPECT_EQ(device::dyn_cast<PowerDevice>(dev.object()), nullptr);
        }
        if(device::dyn_cast<CharDevice>(dev.object()) != nullptr) {
            found_char_device = true;
        }
        if(device::dyn_cast<PowerDevice>(dev.object()) != nullptr) {
            found_power_device = true;
            KTEST_EXPECT_EQ(device::dyn_cast<CharDevice>(dev.object()), nullptr);
        }
    }

    KTEST_EXPECT(found_irq_controller);
    KTEST_EXPECT(found_char_device);
    KTEST_EXPECT(found_power_device);
}

KTEST_CASE(platform, libc_string_primitives) {
    char buf[32]{};
    memset(buf, 'x', 16);
    KTEST_EXPECT_EQ(memcmp(buf, "xxxxxxxxxxxxxxxx", 16), 0);

    memcpy(buf, "0123456789abcdef", 17);
    KTEST_EXPECT_EQ(strlen(buf), size_t{16});
    KTEST_EXPECT_EQ(strcmp(buf, "0123456789abcdef"), 0);
    KTEST_EXPECT(strncmp(buf, "012345999", 6) == 0);
    KTEST_EXPECT(strncmp(buf, "012345999", 7) < 0);

    // Overlapping forward and backward memmove.
    memmove(buf + 2, buf, 10);
    KTEST_EXPECT_EQ(memcmp(buf, "010123456789cdef", 16), 0);
    memmove(buf, buf + 2, 10);
    KTEST_EXPECT_EQ(memcmp(buf, "012345678989cdef", 16), 0);

    KTEST_EXPECT_EQ(memchr(buf, '7', 16), static_cast<void*>(buf + 7));
    KTEST_EXPECT_EQ(memchr(buf, 'z', 16), nullptr);
}

}  // namespace
