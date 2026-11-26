#pragma once

// Schedulable bidirectional character-device interface and active console
// character-device binding for interactive I/O (syscalls).
//
// `CharDevice` is strictly orthogonal to `ConsoleSink` (console_sink.h):
//   - `ConsoleSink` serves non-scheduling kernel log and lock-free panic paths.
//   - `CharDevice` serves schedulable thread-context byte streams (`read` /
//     `write` may sleep on wait queues until hardware interrupts make progress).
//
// Backends live in static storage or the driver arena and are never freed; the
// binding below borrows the probed object pointer for the rest of the kernel.

#include <cstddef>
#include <span>

class CharDevice {
public:
    // Schedulable thread-context read. Blocks until at least one input byte is
    // available, then copies 1..buffer.size() currently available bytes and
    // returns the byte count (0 if `buffer` is empty or panic is latched).
    //
    // Preconditions: current thread is non-idle, interrupts enabled (SIE = 1),
    // and no spinlock held.
    [[nodiscard]] virtual std::size_t read(std::span<char> buffer) noexcept = 0;

    // Schedulable thread-context write. Enqueues all bytes in `data` for
    // transmission, sleeping on backpressure until buffer space becomes
    // available. Returns the number of bytes accepted (`data.size()`, or fewer
    // if panic is latched mid-write).
    //
    // Preconditions: current thread is non-idle, interrupts enabled (SIE = 1),
    // and no spinlock held.
    [[nodiscard]] virtual std::size_t write(std::span<const char> data) noexcept = 0;

protected:
    // Devices live in static storage or the driver arena and are never freed.
    ~CharDevice() = default;
};

namespace char_device {

// Bind `device` as the active interactive console character device used by
// user-space stdio syscalls until a VFS / per-process file descriptor table
// exists. Called by the driver core when a probed object exposes `CharDevice`.
void attach(CharDevice& device) noexcept;

// Borrow the active interactive character device, or nullptr if none is bound.
[[nodiscard]] CharDevice* active() noexcept;

}  // namespace char_device
