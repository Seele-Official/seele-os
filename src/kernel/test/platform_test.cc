#include <cstddef>
#include <cstdint>
#include <span>
#include <string.h>

#include "char_device.h"
#include "console.h"
#include "console_sink.h"
#include "device_object.h"
#include "hart.h"
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
    KTEST_EXPECT(char_device::active() != nullptr);
    KTEST_EXPECT(power::can_poweroff());

    bool found_irq_controller = false;
    bool found_console_sink = false;
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
        if(device::dyn_cast<ConsoleSink>(dev.object()) != nullptr) {
            found_console_sink = true;
        }
        if(device::dyn_cast<CharDevice>(dev.object()) != nullptr) {
            found_char_device = true;
        }
        if(device::dyn_cast<PowerDevice>(dev.object()) != nullptr) {
            found_power_device = true;
            KTEST_EXPECT_EQ(device::dyn_cast<ConsoleSink>(dev.object()), nullptr);
            KTEST_EXPECT_EQ(device::dyn_cast<CharDevice>(dev.object()), nullptr);
        }
    }

    KTEST_EXPECT(found_irq_controller);
    KTEST_EXPECT(found_console_sink);
    KTEST_EXPECT(found_char_device);
    KTEST_EXPECT(found_power_device);
}

KTEST_CASE(platform, char_device_blocking_write) {
    CharDevice* const dev = char_device::active();
    KTEST_ASSERT(dev != nullptr);

    // Exceed Ns16550's 128-byte TX ring so this exercises the WaitQueue sleep
    // and UART TX-empty IRQ wakeup path end to end.
    constexpr size_t PAYLOAD_SIZE = 320;
    char payload[PAYLOAD_SIZE]{};
    memset(payload, '.', PAYLOAD_SIZE - 1);
    memcpy(payload, "[ktest-char] ", 13);
    payload[PAYLOAD_SIZE - 1] = '\n';

    KTEST_EXPECT_EQ(dev->write(std::span<const char>{payload, PAYLOAD_SIZE}), PAYLOAD_SIZE);
}

KTEST_SMP_CASE(platform, smp_log_and_char_write) {
    CharDevice* const dev = char_device::active();
    KTEST_ASSERT(dev != nullptr);

    ktest::barrier();

    if(cpuid() == 0) {
        constexpr size_t PAYLOAD_SIZE = 256;
        char payload[PAYLOAD_SIZE]{};
        memset(payload, '-', PAYLOAD_SIZE - 1);
        memcpy(payload, "[ktest-smp-char] ", 17);
        payload[PAYLOAD_SIZE - 1] = '\n';

        for(int round = 0; round < 3; ++round) {
            KTEST_EXPECT_EQ(dev->write(std::span<const char>{payload, PAYLOAD_SIZE}), PAYLOAD_SIZE);
        }
    } else {
        // Concurrent synchronous kernel logs drain Ns16550's async TX ring
        // under console::g_lock -> m_lock while keeping IER_TX_EMPTY armed for
        // any blocked writer on hart 0.
        for(int round = 0; round < 3; ++round) {
            LOG_DEBUG("ktest: smp sync log hart {} round {}", cpuid(), round);
        }
    }

    ktest::barrier();
}

KTEST_SMP_CASE(platform, smp_char_device_concurrent_write) {
    CharDevice* const dev = char_device::active();
    KTEST_ASSERT(dev != nullptr);

    // Exceed Ns16550's 128-byte TX ring from every hart concurrently so
    // `m_tx_writer` (Mutex) serializes multi-chunk writers across harts.
    constexpr size_t PAYLOAD_SIZE = 160;
    char payload[PAYLOAD_SIZE]{};
    const char fill = static_cast<char>('0' + (cpuid() % 10));
    memset(payload, fill, PAYLOAD_SIZE - 1);
    memcpy(payload, "[ktest-mtx-tx] ", 15);
    payload[PAYLOAD_SIZE - 1] = '\n';

    ktest::barrier();
    KTEST_EXPECT_EQ(dev->write(std::span<const char>{payload, PAYLOAD_SIZE}), PAYLOAD_SIZE);
    ktest::barrier();
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
