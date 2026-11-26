#include "crt/io.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "crt/syscall.h"
#include "uapi/syscall.h"
#include "util/format.h"

namespace user {

void write_all(uint64_t fd, std::string_view text) noexcept {
    std::size_t offset = 0;
    while(offset < text.size()) {
        const int64_t written = sys::write(fd, text.data() + offset, text.size() - offset);
        if(written <= 0) {
            return;
        }
        offset += static_cast<std::size_t>(written);
    }
}

void FdSink::write(std::string_view text) noexcept {
    for(char c: text) {
        m_stage[m_count++] = c;
        if(m_count == STAGE_SIZE) {
            flush();
        }
    }
}

void FdSink::flush() noexcept {
    if(m_count == 0) {
        return;
    }
    write_all(m_fd, std::string_view{m_stage, m_count});
    m_count = 0;
}

void vprint(std::string_view fmt, format_args args) noexcept {
    FdSink out{uapi::STDOUT_FD};
    ::vformat_to(out, fmt, args);
}

void vprintln(std::string_view fmt, format_args args) noexcept {
    FdSink out{uapi::STDOUT_FD};
    ::vformat_to(out, fmt, args);
    out.putc('\n');
}

}  // namespace user
