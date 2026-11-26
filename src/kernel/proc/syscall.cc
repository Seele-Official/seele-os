#include "proc/syscall.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "console.h"
#include "hart.h"
#include "panic.h"
#include "power.h"
#include "riscv.h"
#include "mm/addr.h"
#include "mm/vm.h"
#include "proc/process.h"
#include "sched/schedule.h"
#include "uapi/syscall.h"

namespace syscall {
namespace {

constexpr size_t MAX_WRITE_BYTES = 4096;
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
    if(!console::can_read()) {
        return uapi::err::NODEV;
    }

    const Paddr pt = current_pagetable();
    const Vaddr buf_va = vaddr(buf_addr);
    // Validate the declared user range and confirm the first target byte is
    // writable before blocking so an invalid pointer never swallows input.
    if(!vm::u::is_user_range(buf_va, count) ||
       !vm::u::check_user_range(pt, buf_va, 1, riscv::PTE_R | riscv::PTE_W)) {
        return uapi::err::FAULT;
    }

    const char ch = console::read();
    if(!vm::u::copy_to_user(pt, buf_va, &ch, 1)) {
        return uapi::err::FAULT;
    }
    return 1;
}

[[nodiscard]] int64_t sys_write(uint64_t fd, uint64_t buf_addr, uint64_t count) noexcept {
    if(fd != uapi::STDOUT_FD && fd != uapi::STDERR_FD) {
        return uapi::err::BADF;
    }
    if(count == 0) {
        return 0;
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
        console::write(std::string_view{stage, chunk});
        done += chunk;
    }

    return static_cast<int64_t>(done);
}

[[noreturn]] void sys_exit(uint64_t status) noexcept {
    proc::thread* const current = mycpu()->current;
    SEELE_ASSERT(current != nullptr, "syscall: exit without current thread");

    const auto code = static_cast<int64_t>(status);
    current->exit_code.store(code, std::memory_order_release);
    LOG_DEBUG("syscall: user thread {:#018x} exited with status {}",
              to_uint64(vaddr(current)),
              code);

    riscv::intr_off();
    sched::exit_current();
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
        case uapi::sys::EXIT: sys_exit(frame->a0);
        case uapi::sys::SHUTDOWN: result = sys_shutdown(frame->a0); break;
        default:
            LOG_WARN("syscall: unknown number {} at sepc={:#018x}", frame->a7, frame->sepc - 4);
            result = uapi::err::NOSYS;
            break;
    }

    frame->a0 = static_cast<uint64_t>(result);
}

}  // namespace syscall
