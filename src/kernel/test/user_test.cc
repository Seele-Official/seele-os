#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string.h>

#include "riscv.h"
#include "mm/addr.h"
#include "mm/kalloc.h"
#include "mm/vm.h"
#include "platform/platform.h"
#include "proc/elf_format.h"
#include "proc/elf_loader.h"
#include "proc/process.h"
#include "proc/shell.h"
#include "sched/schedule.h"
#include "test/ktest.h"
#include "uapi/syscall.h"

namespace {

// Minimal RV64I encoders for self-contained U-mode test payloads.
constexpr uint32_t rv_addi(uint32_t rd, uint32_t rs1, int32_t imm) noexcept {
    return ((static_cast<uint32_t>(imm) & 0xfffu) << 20) | (rs1 << 15) | (0u << 12) | (rd << 7) |
           0x13u;
}

constexpr uint32_t rv_lui(uint32_t rd, uint32_t imm20) noexcept {
    return (imm20 << 12) | (rd << 7) | 0x37u;
}

constexpr uint32_t rv_auipc(uint32_t rd, uint32_t imm20) noexcept {
    return (imm20 << 12) | (rd << 7) | 0x17u;
}

constexpr uint32_t rv_ld(uint32_t rd, uint32_t rs1, int32_t imm) noexcept {
    return ((static_cast<uint32_t>(imm) & 0xfffu) << 20) | (rs1 << 15) | (3u << 12) | (rd << 7) |
           0x03u;
}

constexpr uint32_t rv_sd(uint32_t rs2, uint32_t rs1, int32_t imm) noexcept {
    const uint32_t u = static_cast<uint32_t>(imm) & 0xfffu;
    const uint32_t hi = (u >> 5) & 0x7fu;
    const uint32_t lo = u & 0x1fu;
    return (hi << 25) | (rs2 << 20) | (rs1 << 15) | (3u << 12) | (lo << 7) | 0x23u;
}

constexpr uint32_t RV_ECALL = 0x00000073u;

// RISC-V register numbers used in the test snippets.
constexpr uint32_t X0 = 0;
constexpr uint32_t T0 = 5;
constexpr uint32_t T1 = 6;
constexpr uint32_t S1 = 9;
constexpr uint32_t A0 = 10;
constexpr uint32_t A1 = 11;
constexpr uint32_t A2 = 12;
constexpr uint32_t A7 = 17;

[[nodiscard]] bool wait_until_exited(proc::thread* thread) noexcept {
    const uint64_t hz = platform::info().timebase_frequency() != 0
                            ? platform::info().timebase_frequency()
                            : 10'000'000ull;
    const uint64_t deadline = riscv::csrr(riscv::registers::time) + hz * 2;

    while(thread->state.load(std::memory_order_acquire) != proc::ThreadState::Exited) {
        if(riscv::csrr(riscv::registers::time) >= deadline) {
            return false;
        }
        riscv::intr_off();
        sched::yield();
        riscv::intr_on();
    }
    return true;
}

void cleanup_exited_user_proc(proc::process* proc_obj, proc::thread* thread_obj) noexcept {
    const Paddr pt = proc_obj->pagetable;
    vm::k::free_kernel_stack(thread_obj->kstack.base);
    delete thread_obj;
    delete proc_obj;
    vm::u::destroy_user_mappings(pt);
    phymem::free(pt);
}

KTEST_CASE(user, embedded_shell_elf_load) {
    const std::span<const std::byte> image = proc::shell_image();
    KTEST_ASSERT(image.size() > sizeof(elf::elf64_ehdr));

    const Paddr pt = vm::k::create_process_pagetable();
    KTEST_ASSERT(!is_null(pt));

    const std::optional<elf::loaded_user_image> loaded = elf::load_user_elf(pt, image);
    KTEST_ASSERT(loaded.has_value());
    KTEST_EXPECT(range_contains(vm::u::USER_BASE, vm::u::USER_LOAD_LIMIT, loaded->entry));
    KTEST_EXPECT_EQ(loaded->user_sp, vm::u::USER_STACK_TOP);

    // Entry page must be executable and readable from U-mode, never writable (W^X).
    KTEST_EXPECT(vm::u::translate_user(pt, loaded->entry, riscv::PTE_R | riscv::PTE_X).has_value());
    KTEST_EXPECT(!vm::u::translate_user(pt, loaded->entry, riscv::PTE_W).has_value());

    // User stack must be RW/NX with unmapped guard pages both below and above.
    const Vaddr top_stack_byte = vm::u::USER_STACK_TOP - 8;
    KTEST_EXPECT(
        vm::u::translate_user(pt, top_stack_byte, riscv::PTE_R | riscv::PTE_W).has_value());
    KTEST_EXPECT(!vm::u::translate_user(pt, top_stack_byte, riscv::PTE_X).has_value());
    KTEST_EXPECT(!vm::u::translate_user(pt, vm::u::USER_STACK_BASE - riscv::PGSIZE, riscv::PTE_R)
                      .has_value());
    KTEST_EXPECT(!vm::u::translate_user(pt, vm::u::USER_STACK_TOP, riscv::PTE_R).has_value());

    vm::u::destroy_user_mappings(pt);
    phymem::free(pt);
}

KTEST_CASE(user, umode_syscalls_and_exit) {
    const Paddr pt = vm::k::create_process_pagetable();
    KTEST_ASSERT(!is_null(pt));

    const Paddr code_pa = phymem::alloc();
    const Paddr data_pa = phymem::alloc();
    const Paddr stack_pa = phymem::alloc();
    KTEST_ASSERT(!is_null(code_pa) && !is_null(data_pa) && !is_null(stack_pa));

    auto* const code_dst = to_pointer<uint32_t>(vm::k::P2V(code_pa));
    auto* const data_dst = to_pointer<char>(vm::k::P2V(data_pa));
    memset(code_dst, 0, riscv::PGSIZE);
    memset(data_dst, 0, riscv::PGSIZE);
    memset(to_pointer<void>(vm::k::P2V(stack_pa)), 0, riscv::PGSIZE);

    static constexpr char MSG[] = "[ktest-user] ok\n";
    constexpr int32_t MSG_LEN = static_cast<int32_t>(sizeof(MSG) - 1);
    memcpy(data_dst, MSG, sizeof(MSG) - 1);

    constexpr Vaddr CODE_VA = vm::u::USER_BASE + 0x10000;
    constexpr Vaddr DATA_VA = vm::u::USER_BASE + 0x20000;
    constexpr Vaddr STACK_PAGE_VA = vm::u::USER_STACK_TOP - riscv::PGSIZE;

    // Straight-line U-mode program starting at CODE_VA:
    //   s1 = DATA_VA (= CODE_VA + 0x10000)
    //   [64(s1)] = write(STDOUT_FD, s1, MSG_LEN)     -> expected MSG_LEN
    //   [72(s1)] = write(9, s1, 4)                   -> expected err::BADF
    //   [80(s1)] = write(STDOUT_FD, 0x10000000, 4)   -> expected err::FAULT
    //   [88(s1)] = syscall(777)                      -> expected err::NOSYS
    //   exit(42)
    const uint32_t prog[] = {
        rv_auipc(S1, 0x10),

        rv_addi(A0, X0, static_cast<int32_t>(uapi::STDOUT_FD)),
        rv_addi(A1, S1, 0),
        rv_addi(A2, X0, MSG_LEN),
        rv_addi(A7, X0, static_cast<int32_t>(uapi::sys::WRITE)),
        RV_ECALL,
        rv_sd(A0, S1, 64),

        rv_addi(A0, X0, 9),
        rv_addi(A1, S1, 0),
        rv_addi(A2, X0, 4),
        rv_addi(A7, X0, static_cast<int32_t>(uapi::sys::WRITE)),
        RV_ECALL,
        rv_sd(A0, S1, 72),

        rv_addi(A0, X0, static_cast<int32_t>(uapi::STDOUT_FD)),
        rv_lui(A1, 0x10000),
        rv_addi(A2, X0, 4),
        rv_addi(A7, X0, static_cast<int32_t>(uapi::sys::WRITE)),
        RV_ECALL,
        rv_sd(A0, S1, 80),

        rv_addi(A0, X0, 0),
        rv_addi(A7, X0, 777),
        RV_ECALL,
        rv_sd(A0, S1, 88),

        rv_addi(A0, X0, 42),
        rv_addi(A7, X0, static_cast<int32_t>(uapi::sys::EXIT)),
        RV_ECALL,
    };
    memcpy(code_dst, prog, sizeof(prog));

    KTEST_ASSERT(
        vm::u::map_user_pages(pt, CODE_VA, riscv::PGSIZE, code_pa, riscv::PTE_R | riscv::PTE_X));
    KTEST_ASSERT(
        vm::u::map_user_pages(pt, DATA_VA, riscv::PGSIZE, data_pa, riscv::PTE_R | riscv::PTE_W));
    KTEST_ASSERT(vm::u::map_user_pages(pt,
                                       STACK_PAGE_VA,
                                       riscv::PGSIZE,
                                       stack_pa,
                                       riscv::PTE_R | riscv::PTE_W));
    riscv::fence_i();

    proc::process* const uproc = proc::make_proc(pt);
    proc::thread* const uthread = proc::make_user_thread(uproc, CODE_VA, vm::u::USER_STACK_TOP);
    sched::enqueue(uthread);

    KTEST_ASSERT(wait_until_exited(uthread));
    KTEST_EXPECT_EQ(uthread->exit_code.load(std::memory_order_acquire), int64_t{42});
    KTEST_EXPECT_EQ(uthread->fault_cause.load(std::memory_order_acquire), uint64_t{0});

    int64_t results[4]{};
    KTEST_ASSERT(vm::u::copy_from_user(pt, results, DATA_VA + 64, sizeof(results)));
    KTEST_EXPECT_EQ(results[0], static_cast<int64_t>(MSG_LEN));
    KTEST_EXPECT_EQ(results[1], uapi::err::BADF);
    KTEST_EXPECT_EQ(results[2], uapi::err::FAULT);
    KTEST_EXPECT_EQ(results[3], uapi::err::NOSYS);

    cleanup_exited_user_proc(uproc, uthread);
}

KTEST_CASE(user, umode_kernel_access_faults) {
    const Paddr pt = vm::k::create_process_pagetable();
    KTEST_ASSERT(!is_null(pt));

    const Paddr code_pa = phymem::alloc();
    const Paddr stack_pa = phymem::alloc();
    KTEST_ASSERT(!is_null(code_pa) && !is_null(stack_pa));

    auto* const code_dst = to_pointer<uint32_t>(vm::k::P2V(code_pa));
    memset(code_dst, 0, riscv::PGSIZE);
    memset(to_pointer<void>(vm::k::P2V(stack_pa)), 0, riscv::PGSIZE);

    constexpr Vaddr CODE_VA = vm::u::USER_BASE + 0x10000;
    constexpr Vaddr STACK_PAGE_VA = vm::u::USER_STACK_TOP - riscv::PGSIZE;

    // Attempt to read supervisor-only direct-map MMIO/RAM (0x1000_0000) from
    // U-mode. Hardware must raise LOAD_PAGE_FAULT and usertrap must retire the
    // faulting thread without panicking the kernel.
    const uint32_t prog[] = {
        rv_lui(T0, 0x10000),
        rv_ld(T1, T0, 0),
        rv_addi(A0, X0, 99),
        rv_addi(A7, X0, static_cast<int32_t>(uapi::sys::EXIT)),
        RV_ECALL,
    };
    memcpy(code_dst, prog, sizeof(prog));

    KTEST_ASSERT(
        vm::u::map_user_pages(pt, CODE_VA, riscv::PGSIZE, code_pa, riscv::PTE_R | riscv::PTE_X));
    KTEST_ASSERT(vm::u::map_user_pages(pt,
                                       STACK_PAGE_VA,
                                       riscv::PGSIZE,
                                       stack_pa,
                                       riscv::PTE_R | riscv::PTE_W));
    riscv::fence_i();

    proc::process* const uproc = proc::make_proc(pt);
    proc::thread* const uthread = proc::make_user_thread(uproc, CODE_VA, vm::u::USER_STACK_TOP);
    sched::enqueue(uthread);

    KTEST_ASSERT(wait_until_exited(uthread));
    KTEST_EXPECT_EQ(uthread->fault_cause.load(std::memory_order_acquire),
                    riscv::csr::scause::LOAD_PAGE_FAULT);
    KTEST_EXPECT_EQ(uthread->exit_code.load(std::memory_order_acquire), int64_t{-1});

    cleanup_exited_user_proc(uproc, uthread);
}

}  // namespace
