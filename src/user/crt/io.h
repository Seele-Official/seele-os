#pragma once

// User-space formatted and raw console output built on the shared `util/format.h`
// engine and `user::sys::write()`.

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "util/format.h"

namespace user {

// Write all bytes of `text` to `fd`, retrying short writes until complete or
// an error occurs.
void write_all(uint64_t fd, std::string_view text) noexcept;

// Buffered FormatSink targeting a user file descriptor (typically STDOUT_FD or
// STDERR_FD). Stages formatted chunks on the stack and flushes on overflow and
// destruction without heap allocation.
class FdSink final : public FormatSink {
public:
    explicit FdSink(uint64_t fd) noexcept : m_fd(fd) {}

    ~FdSink() noexcept {
        flush();
    }

    void write(std::string_view text) noexcept override;
    void flush() noexcept;

private:
    static constexpr std::size_t STAGE_SIZE = 64;

    uint64_t m_fd;
    char m_stage[STAGE_SIZE]{};
    std::size_t m_count = 0;
};

void vprint(std::string_view fmt, format_args args) noexcept;
void vprintln(std::string_view fmt, format_args args) noexcept;

template <typename... Args>
void print(format_string<Args...> fmt, Args&&... args) noexcept {
    vprint(fmt.get(), ::make_format_args(args...));
}

template <typename... Args>
void println(format_string<Args...> fmt, Args&&... args) noexcept {
    vprintln(fmt.get(), ::make_format_args(args...));
}

}  // namespace user
