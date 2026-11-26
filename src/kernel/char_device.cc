#include "char_device.h"

#include <atomic>

namespace {

// Borrowed pointer to the active interactive character device (driver-arena or
// static storage, never freed). Published during boot probe on the boot hart.
std::atomic<CharDevice*> g_active{nullptr};

}  // namespace

namespace char_device {

void attach(CharDevice& device) noexcept {
    g_active.store(&device, std::memory_order_release);
}

CharDevice* active() noexcept {
    return g_active.load(std::memory_order_acquire);
}

}  // namespace char_device
