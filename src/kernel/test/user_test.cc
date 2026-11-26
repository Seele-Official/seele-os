#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string.h>

#include "riscv.h"
#include "concurrency/semaphore.h"
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

constexpr uint32_t rv_beq(uint32_t rs1, uint32_t rs2, int32_t offset) noexcept {
    const uint32_t u = static_cast<uint32_t>(offset);
    const uint32_t imm12 = (u >> 12) & 0x1u;
    const uint32_t imm10_5 = (u >> 5) & 0x3fu;
    const uint32_t imm4_1 = (u >> 1) & 0xfu;
    const uint32_t imm11 = (u >> 11) & 0x1u;
    return (imm12 << 31) | (imm10_5 << 25) | (rs2 << 20) | (rs1 << 15) | (0u << 12) |
           (imm4_1 << 8) | (imm11 << 7) | 0x63u;
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

constexpr Vaddr TEST_CODE_VA = vm::u::USER_BASE + 0x10000;
constexpr Vaddr TEST_DATA_VA = vm::u::USER_BASE + 0x20000;
constexpr Vaddr TEST_STACK_PAGE_VA = vm::u::USER_STACK_TOP - riscv::PGSIZE;

[[nodiscard]] uint64_t two_second_deadline() noexcept {
    const uint64_t hz = platform::info().timebase_frequency() != 0
                            ? platform::info().timebase_frequency()
                            : 10'000'000ull;
    return riscv::csrr(riscv::registers::time) + hz * 2;
}

template <typename Pred>
[[nodiscard]] bool wait_with_yield(Pred&& predicate) noexcept {
    const uint64_t deadline = two_second_deadline();
    while(!predicate()) {
        if(riscv::csrr(riscv::registers::time) >= deadline) {
            return false;
        }
        riscv::intr_off();
        sched::yield();
        riscv::intr_on();
    }
    return true;
}

// Build a minimal U-mode process page table containing a code page at
// `TEST_CODE_VA` and a 1-page stack at `TEST_STACK_PAGE_VA`.
[[nodiscard]] Paddr make_simple_user_pagetable(std::span<const uint32_t> code) noexcept {
    const Paddr pt = vm::k::create_process_pagetable();
    const Paddr code_pa = phymem::alloc();
    const Paddr stack_pa = phymem::alloc();
    if(is_null(pt) || is_null(code_pa) || is_null(stack_pa)) {
        return null_paddr();
    }

    auto* const code_dst = to_pointer<uint32_t>(vm::k::P2V(code_pa));
    memset(code_dst, 0, riscv::PGSIZE);
    memset(to_pointer<void>(vm::k::P2V(stack_pa)), 0, riscv::PGSIZE);
    memcpy(code_dst, code.data(), code.size_bytes());

    if(!vm::u::map_user_pages(pt,
                              TEST_CODE_VA,
                              riscv::PGSIZE,
                              code_pa,
                              riscv::PTE_R | riscv::PTE_X) ||
       !vm::u::map_user_pages(pt,
                              TEST_STACK_PAGE_VA,
                              riscv::PGSIZE,
                              stack_pa,
                              riscv::PTE_R | riscv::PTE_W)) {
        vm::u::destroy_user_mappings(pt);
        phymem::free(pt);
        return null_paddr();
    }

    riscv::fence_i();
    return pt;
}

Semaphore g_kthread_ran{0};

void kthread_self_exit_fn() noexcept {
    g_kthread_ran.release();
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
    const uint64_t baseline_procs = proc::user_process_count();

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

    // Payload exceeding Ns16550's 128-byte TX ring and syscall's 128-byte
    // staging chunk to exercise schedulable blocking write from U-mode.
    constexpr int32_t BULK_OFFSET = 128;
    constexpr int32_t BULK_LEN = 260;
    memset(data_dst + BULK_OFFSET, '=', BULK_LEN - 1);
    memcpy(data_dst + BULK_OFFSET, "[ktest-user-bulk] ", 18);
    data_dst[BULK_OFFSET + BULK_LEN - 1] = '\n';

    // Straight-line U-mode program starting at TEST_CODE_VA:
    //   s1 = TEST_DATA_VA (= TEST_CODE_VA + 0x10000)
    //   1. write(STDOUT_FD, s1, MSG_LEN)        == MSG_LEN  (else exit 101)
    //   2. write(9, s1, 4)                      == BADF     (else exit 102)
    //   3. write(STDOUT_FD, 0x10000000, 4)      == FAULT    (else exit 103)
    //   4. syscall(777)                         == NOSYS    (else exit 104)
    //   5. write(STDOUT_FD, s1 + 128, BULK_LEN) == BULK_LEN (else exit 105)
    //   exit_group(42)
    // Each check uses `beq a0, t1, +16` to skip the 3-instruction fail exit.
    constexpr int32_t EXIT_NR = static_cast<int32_t>(uapi::sys::EXIT_GROUP);
    const uint32_t prog[] = {
        rv_auipc(S1, 0x10),

        rv_addi(A0, X0, static_cast<int32_t>(uapi::STDOUT_FD)),
        rv_addi(A1, S1, 0),
        rv_addi(A2, X0, MSG_LEN),
        rv_addi(A7, X0, static_cast<int32_t>(uapi::sys::WRITE)),
        RV_ECALL,
        rv_addi(T1, X0, MSG_LEN),
        rv_beq(A0, T1, 16),
        rv_addi(A0, X0, 101),
        rv_addi(A7, X0, EXIT_NR),
        RV_ECALL,

        rv_addi(A0, X0, 9),
        rv_addi(A1, S1, 0),
        rv_addi(A2, X0, 4),
        rv_addi(A7, X0, static_cast<int32_t>(uapi::sys::WRITE)),
        RV_ECALL,
        rv_addi(T1, X0, static_cast<int32_t>(uapi::err::BADF)),
        rv_beq(A0, T1, 16),
        rv_addi(A0, X0, 102),
        rv_addi(A7, X0, EXIT_NR),
        RV_ECALL,

        rv_addi(A0, X0, static_cast<int32_t>(uapi::STDOUT_FD)),
        rv_lui(A1, 0x10000),
        rv_addi(A2, X0, 4),
        rv_addi(A7, X0, static_cast<int32_t>(uapi::sys::WRITE)),
        RV_ECALL,
        rv_addi(T1, X0, static_cast<int32_t>(uapi::err::FAULT)),
        rv_beq(A0, T1, 16),
        rv_addi(A0, X0, 103),
        rv_addi(A7, X0, EXIT_NR),
        RV_ECALL,

        rv_addi(A0, X0, 0),
        rv_addi(A7, X0, 777),
        RV_ECALL,
        rv_addi(T1, X0, static_cast<int32_t>(uapi::err::NOSYS)),
        rv_beq(A0, T1, 16),
        rv_addi(A0, X0, 104),
        rv_addi(A7, X0, EXIT_NR),
        RV_ECALL,

        rv_addi(A0, X0, static_cast<int32_t>(uapi::STDOUT_FD)),
        rv_addi(A1, S1, BULK_OFFSET),
        rv_addi(A2, X0, BULK_LEN),
        rv_addi(A7, X0, static_cast<int32_t>(uapi::sys::WRITE)),
        RV_ECALL,
        rv_addi(T1, X0, BULK_LEN),
        rv_beq(A0, T1, 16),
        rv_addi(A0, X0, 105),
        rv_addi(A7, X0, EXIT_NR),
        RV_ECALL,

        rv_addi(A0, X0, 42),
        rv_addi(A7, X0, EXIT_NR),
        RV_ECALL,
    };
    memcpy(code_dst, prog, sizeof(prog));

    KTEST_ASSERT(vm::u::map_user_pages(pt,
                                       TEST_CODE_VA,
                                       riscv::PGSIZE,
                                       code_pa,
                                       riscv::PTE_R | riscv::PTE_X));
    KTEST_ASSERT(vm::u::map_user_pages(pt,
                                       TEST_DATA_VA,
                                       riscv::PGSIZE,
                                       data_pa,
                                       riscv::PTE_R | riscv::PTE_W));
    KTEST_ASSERT(vm::u::map_user_pages(pt,
                                       TEST_STACK_PAGE_VA,
                                       riscv::PGSIZE,
                                       stack_pa,
                                       riscv::PTE_R | riscv::PTE_W));
    riscv::fence_i();

    proc::ProcessHandle uproc = proc::make_proc(pt);
    KTEST_EXPECT_EQ(proc::user_process_count(), baseline_procs + 1);
    proc::thread* const uthread =
        proc::make_user_thread(uproc.get(), TEST_CODE_VA, vm::u::USER_STACK_TOP);
    sched::enqueue(uthread);

    const proc::exit_status status = uproc.wait();
    KTEST_EXPECT_EQ(status.code, int64_t{42});
    KTEST_EXPECT_EQ(status.fault_cause, uint64_t{0});

    const std::optional<proc::exit_status> polled = uproc.try_status();
    KTEST_ASSERT(polled.has_value());
    KTEST_EXPECT_EQ(polled->code, int64_t{42});
    KTEST_EXPECT_EQ(polled->fault_cause, uint64_t{0});
    KTEST_EXPECT(is_null(uproc->pagetable));
    KTEST_EXPECT_EQ(uproc->satp, uint64_t{0});
    KTEST_EXPECT_EQ(uproc->thread_count.load(std::memory_order_acquire), uint64_t{0});

    uproc.reset();
    KTEST_EXPECT_EQ(proc::user_process_count(), baseline_procs);
}

KTEST_CASE(user, umode_kernel_access_faults) {
    const uint64_t baseline_procs = proc::user_process_count();

    // Attempt to read supervisor-only direct-map MMIO/RAM (0x1000_0000) from
    // U-mode. Hardware must raise LOAD_PAGE_FAULT and usertrap must terminate
    // the user process cleanly without panicking the kernel.
    const uint32_t prog[] = {
        rv_lui(T0, 0x10000),
        rv_ld(T1, T0, 0),
        rv_addi(A0, X0, 99),
        rv_addi(A7, X0, static_cast<int32_t>(uapi::sys::EXIT_GROUP)),
        RV_ECALL,
    };
    const Paddr pt = make_simple_user_pagetable(prog);
    KTEST_ASSERT(!is_null(pt));

    proc::ProcessHandle uproc = proc::make_proc(pt);
    proc::thread* const uthread =
        proc::make_user_thread(uproc.get(), TEST_CODE_VA, vm::u::USER_STACK_TOP);
    sched::enqueue(uthread);

    const proc::exit_status status = uproc.wait();
    KTEST_EXPECT_EQ(status.fault_cause, riscv::csr::scause::LOAD_PAGE_FAULT);
    KTEST_EXPECT_EQ(status.code, int64_t{-1});
    KTEST_EXPECT(is_null(uproc->pagetable));
    KTEST_EXPECT_EQ(uproc->thread_count.load(std::memory_order_acquire), uint64_t{0});

    uproc.reset();
    KTEST_EXPECT_EQ(proc::user_process_count(), baseline_procs);
}

KTEST_CASE(user, process_reclamation_and_detached_exit) {
    const uint64_t baseline_procs = proc::user_process_count();

    // 1. A user process whose handle is dropped before any thread is created
    //    must still reclaim its pagetable and user mappings on destruction.
    {
        const uint32_t dummy_prog[] = {
            rv_addi(A0, X0, 0),
            rv_addi(A7, X0, static_cast<int32_t>(uapi::sys::EXIT_GROUP)),
            RV_ECALL,
        };
        const Paddr pt = make_simple_user_pagetable(dummy_prog);
        KTEST_ASSERT(!is_null(pt));

        proc::ProcessHandle empty_proc = proc::make_proc(pt);
        KTEST_EXPECT_EQ(proc::user_process_count(), baseline_procs + 1);
        KTEST_EXPECT(!empty_proc.try_status().has_value());
    }
    KTEST_EXPECT_EQ(proc::user_process_count(), baseline_procs);

    // 2. Shared ProcessHandle copies keep the lightweight process stub alive
    //    until the final handle drops, while the thread's kstack slot and user
    //    pagetable are reclaimed immediately on exit.
    {
        const uint32_t prog[] = {
            rv_addi(A0, X0, 7),
            rv_addi(A7, X0, static_cast<int32_t>(uapi::sys::EXIT_GROUP)),
            RV_ECALL,
        };
        const Paddr pt = make_simple_user_pagetable(prog);
        KTEST_ASSERT(!is_null(pt));

        proc::ProcessHandle h1 = proc::make_proc(pt);
        proc::ProcessHandle h2 = h1;
        proc::thread* const uthread =
            proc::make_user_thread(h1.get(), TEST_CODE_VA, vm::u::USER_STACK_TOP);
        const Vaddr saved_kstack = uthread->kstack.base;
        sched::enqueue(uthread);

        const proc::exit_status st1 = h1.wait();
        KTEST_EXPECT_EQ(st1.code, int64_t{7});
        KTEST_EXPECT_EQ(st1.fault_cause, uint64_t{0});
        KTEST_EXPECT(is_null(h1->pagetable));

        // The exited thread's kstack slot must already be at the top of the
        // LIFO free list right after wait() returns.
        const std::optional<Vaddr> reused = vm::k::alloc_kernel_stack();
        KTEST_ASSERT(reused.has_value());
        KTEST_EXPECT_EQ(*reused, saved_kstack);
        vm::k::free_kernel_stack(*reused);

        h1.reset();
        KTEST_EXPECT_EQ(proc::user_process_count(), baseline_procs + 1);

        const proc::exit_status st2 = h2.wait();
        KTEST_EXPECT_EQ(st2.code, int64_t{7});
        KTEST_EXPECT_EQ(st2.fault_cause, uint64_t{0});
    }
    KTEST_EXPECT_EQ(proc::user_process_count(), baseline_procs);

    // 3. A detached user process (handle released before running, matching
    //    spawn_shell()) auto-reclaims both its thread and process object on
    //    exit without leaving a zombie behind.
    {
        const uint32_t prog[] = {
            rv_addi(A0, X0, 0),
            rv_addi(A7, X0, static_cast<int32_t>(uapi::sys::EXIT_GROUP)),
            RV_ECALL,
        };
        const Paddr pt = make_simple_user_pagetable(prog);
        KTEST_ASSERT(!is_null(pt));

        proc::ProcessHandle detached = proc::make_proc(pt);
        proc::thread* const uthread =
            proc::make_user_thread(detached.get(), TEST_CODE_VA, vm::u::USER_STACK_TOP);
        const Vaddr saved_kstack = uthread->kstack.base;
        detached.reset();
        KTEST_EXPECT_EQ(proc::user_process_count(), baseline_procs + 1);
        sched::enqueue(uthread);

        KTEST_ASSERT(wait_with_yield(
            [&]() noexcept { return proc::user_process_count() == baseline_procs; }));

        const std::optional<Vaddr> reused = vm::k::alloc_kernel_stack();
        KTEST_ASSERT(reused.has_value());
        KTEST_EXPECT_EQ(*reused, saved_kstack);
        vm::k::free_kernel_stack(*reused);
    }

    // 4. A kernel thread that returns from its entry function retires through
    //    kernel_thread_trampoline and immediately recycles its kstack slot.
    {
        const uint64_t kthreads_before =
            proc::kernel_proc()->thread_count.load(std::memory_order_acquire);
        proc::thread* const kt = proc::make_thread(proc::kernel_proc(), &kthread_self_exit_fn);
        const Vaddr saved_kstack = kt->kstack.base;
        KTEST_EXPECT_EQ(proc::kernel_proc()->thread_count.load(std::memory_order_acquire),
                        kthreads_before + 1);

        sched::enqueue(kt);
        g_kthread_ran.acquire();

        KTEST_ASSERT(wait_with_yield([&]() noexcept {
            return proc::kernel_proc()->thread_count.load(std::memory_order_acquire) ==
                   kthreads_before;
        }));

        const std::optional<Vaddr> reused = vm::k::alloc_kernel_stack();
        KTEST_ASSERT(reused.has_value());
        KTEST_EXPECT_EQ(*reused, saved_kstack);
        vm::k::free_kernel_stack(*reused);
    }
}

}  // namespace
