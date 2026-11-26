#include "proc/syscall.h"

#include <cstddef>
#include <cstdint>
#include <span>

#include "char_device.h"
#include "console.h"
#include "hart.h"
#include "panic.h"
#include "power.h"
#include "riscv.h"
#include "mm/addr.h"
#include "mm/vm.h"
#include "proc/process.h"
#include "uapi/syscall.h"

namespace syscall {
namespace {

constexpr size_t MAX_WRITE_BYTES = 4096;
constexpr size_t READ_STAGE_SIZE = 64;
constexpr size_t WRITE_STAGE_SIZE = 128;

[[nodiscard]] Paddr current_pagetable() noexcept {
    proc::thread* const current = mycpu()->current;
    SEELE_ASSERT(current != nullptr && current->proc != nullptr,
                 "syscall: no current thread or process");
    return current->proc->pagetable;
}

[[nodiscard]] int64_t sys_read(uint64_t fd, uint64_t buf_addr, uint64_t count) noexcept {
    if(fd != uapi::STDIN_FD) {
        return uapi::err::BADF;
    }
    if(count == 0) {
        return 0;
    }
    CharDevice* const dev = char_device::active();
    if(dev == nullptr) {
        return uapi::err::NODEV;
    }

    const Paddr pt = current_pagetable();
    const Vaddr buf_va = vaddr(buf_addr);
    if(!vm::u::is_user_range(buf_va, count)) {
        return uapi::err::FAULT;
    }

    // Clamp the staging chunk within the first user page so a single pre-block
    // permission check guarantees `copy_to_user()` will accept every byte read
    // and an invalid trailing page never swallows input.
    const size_t page_rem =
        riscv::PGSIZE - static_cast<size_t>(to_uint64(buf_va) & (riscv::PGSIZE - 1));
    size_t chunk = (count < READ_STAGE_SIZE) ? static_cast<size_t>(count) : READ_STAGE_SIZE;
    if(page_rem < chunk) {
        chunk = page_rem;
    }

    if(!vm::u::check_user_range(pt, buf_va, chunk, riscv::PTE_R | riscv::PTE_W)) {
        return uapi::err::FAULT;
    }

    char stage[READ_STAGE_SIZE]{};
    const size_t received = dev->read(std::span<char>{stage, chunk});
    if(received == 0) {
        return 0;
    }
    if(!vm::u::copy_to_user(pt, buf_va, stage, received)) {
        return uapi::err::FAULT;
    }
    return static_cast<int64_t>(received);
}

[[nodiscard]] int64_t sys_write(uint64_t fd, uint64_t buf_addr, uint64_t count) noexcept {
    if(fd != uapi::STDOUT_FD && fd != uapi::STDERR_FD) {
        return uapi::err::BADF;
    }
    if(count == 0) {
        return 0;
    }
    CharDevice* const dev = char_device::active();
    if(dev == nullptr) {
        return uapi::err::NODEV;
    }

    const Paddr pt = current_pagetable();
    const Vaddr buf_va = vaddr(buf_addr);
    if(!vm::u::is_user_range(buf_va, count)) {
        return uapi::err::FAULT;
    }

    const size_t total = (count > MAX_WRITE_BYTES) ? MAX_WRITE_BYTES : static_cast<size_t>(count);
    char stage[WRITE_STAGE_SIZE]{};
    size_t done = 0;

    while(done < total) {
        const size_t remaining = total - done;
        const size_t chunk = (remaining < WRITE_STAGE_SIZE) ? remaining : WRITE_STAGE_SIZE;
        if(!vm::u::copy_from_user(pt, stage, buf_va + done, chunk)) {
            return done > 0 ? static_cast<int64_t>(done) : uapi::err::FAULT;
        }
        const size_t accepted = dev->write(std::span<const char>{stage, chunk});
        done += accepted;
        if(accepted < chunk) {
            break;
        }
    }

    return static_cast<int64_t>(done);
}

[[noreturn]] void sys_exit_group(uint64_t status) noexcept {
    proc::thread* const current = mycpu()->current;
    SEELE_ASSERT(current != nullptr && current->proc != nullptr,
                 "syscall: exit_group without current thread or process");

    const auto code = static_cast<int64_t>(status);
    LOG_DEBUG("syscall: user process {:#018x} exited with status {}",
              to_uint64(vaddr(current->proc)),
              code);

    proc::exit_current_process(code);
}

[[nodiscard]] int64_t sys_shutdown(uint64_t code) noexcept {
    if(!power::can_poweroff()) {
        return uapi::err::NODEV;
    }
    power::poweroff(static_cast<uint16_t>(code));
}

}  // namespace

void dispatch(trap::trap_frame* frame) noexcept {
    SEELE_ASSERT(frame != nullptr, "syscall::dispatch(nullptr)");

    int64_t result = 0;
    switch(frame->a7) {
        case uapi::sys::READ: result = sys_read(frame->a0, frame->a1, frame->a2); break;
        case uapi::sys::WRITE: result = sys_write(frame->a0, frame->a1, frame->a2); break;
        case uapi::sys::EXIT_GROUP: sys_exit_group(frame->a0);
        case uapi::sys::SHUTDOWN: result = sys_shutdown(frame->a0); break;
        default:
            LOG_WARN("syscall: unknown number {} at sepc={:#018x}", frame->a7, frame->sepc - 4);
            result = uapi::err::NOSYS;
            break;
    }

    frame->a0 = static_cast<uint64_t>(result);
}

}  // namespace syscall
