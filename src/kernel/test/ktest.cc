#include "test/ktest.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "console.h"
#include "hart.h"
#include "panic.h"
#include "power.h"
#include "riscv.h"
#include "concurrency/lock_guard.h"
#include "concurrency/spinlock.h"
#include "platform/platform.h"
#include "sched/schedule.h"

namespace ktest {
namespace {

extern "C" {
    extern const test_case __ktest_start[];
    extern const test_case __ktest_end[];
}

constexpr uint64_t DEFAULT_TIMEBASE_HZ = 10'000'000;
constexpr uint64_t WORKER_WAIT_SECONDS = 5;

// Per-hart worker coordination kept entirely in shared `.bss` / heap memory
// (`proc::thread*`), avoiding reliance on higher-level wait primitives inside
// the test harness itself.
struct worker_slot {
    Spinlock lock{};
    proc::thread* thread = nullptr;
    bool run_requested = false;
};

worker_slot g_workers[MAX_HARTS];
std::atomic<size_t> g_workers_ready{0};
std::atomic<size_t> g_workers_done{0};

std::atomic<const test_case*> g_active_case{nullptr};
Context g_context{};

void wake_worker(size_t hart) noexcept {
    proc::thread* target = nullptr;
    {
        LockGuard guard{g_workers[hart].lock};
        g_workers[hart].run_requested = true;
        target = g_workers[hart].thread;
    }
    sched::wake(target);
}

[[nodiscard]] std::span<const test_case> registered_cases() noexcept {
    const auto begin = reinterpret_cast<uintptr_t>(__ktest_start);
    const auto end = reinterpret_cast<uintptr_t>(__ktest_end);
    SEELE_ASSERT(end >= begin && (end - begin) % sizeof(test_case) == 0,
                 "ktest: misaligned .ktest_cases bounds [{:#x}, {:#x})",
                 begin,
                 end);
    return {__ktest_start, static_cast<size_t>((end - begin) / sizeof(test_case))};
}

// Extract `test=<value>` from `/chosen/bootargs` without string_view::substr(),
// which pulls std::terminate() into freestanding builds. Empty view means "all".
[[nodiscard]] std::string_view parse_filter(std::string_view cmdline) noexcept {
    constexpr std::string_view PREFIX{"test="};
    size_t pos = 0;
    while(pos < cmdline.size()) {
        while(pos < cmdline.size() && cmdline[pos] == ' ') {
            ++pos;
        }
        size_t end = pos;
        while(end < cmdline.size() && cmdline[end] != ' ') {
            ++end;
        }
        const std::string_view token{cmdline.data() + pos, end - pos};
        if(token.size() > PREFIX.size() &&
           std::string_view{token.data(), PREFIX.size()} == PREFIX) {
            const std::string_view value{token.data() + PREFIX.size(),
                                         token.size() - PREFIX.size()};
            return value == "all" ? std::string_view{} : value;
        }
        pos = end;
    }
    return {};
}

// Match either `<suite>` or `<suite>.<name>` / `<suite>:<name>`.
[[nodiscard]] bool matches_filter(const test_case& tc, std::string_view filter) noexcept {
    if(filter.empty()) {
        return true;
    }
    const std::string_view suite{tc.suite};
    const std::string_view name{tc.name};

    size_t sep = 0;
    while(sep < filter.size() && filter[sep] != '.' && filter[sep] != ':') {
        ++sep;
    }
    if(sep == filter.size()) {
        return suite == filter;
    }
    const std::string_view filter_suite{filter.data(), sep};
    const std::string_view filter_name{filter.data() + sep + 1, filter.size() - sep - 1};
    return suite == filter_suite && name == filter_name;
}

void verify_hart_postconditions(const test_case& tc) noexcept {
    SEELE_ASSERT(mycpu()->interrupt_depth == 0,
                 "ktest: {}.{} leaked {} held spinlock(s) on hart {}",
                 tc.suite,
                 tc.name,
                 mycpu()->interrupt_depth,
                 cpuid());
    SEELE_ASSERT(riscv::is_intr_on(),
                 "ktest: {}.{} left S-mode interrupts disabled on hart {}",
                 tc.suite,
                 tc.name,
                 cpuid());
}

[[nodiscard]] uint64_t timeout_deadline() noexcept {
    const uint64_t hz = platform::info().timebase_frequency() != 0
                            ? platform::info().timebase_frequency()
                            : DEFAULT_TIMEBASE_HZ;
    return riscv::csrr(riscv::registers::time) + hz * WORKER_WAIT_SECONDS;
}

[[nodiscard]] size_t wait_for_workers() noexcept {
    const size_t discovered = platform::info().harts().size();
    const size_t online_target =
        discovered == 0 ? 1 : (discovered < MAX_HARTS ? discovered : MAX_HARTS);
    const size_t expected_workers = online_target - 1;
    if(expected_workers == 0) {
        return 1;
    }

    const uint64_t deadline = timeout_deadline();
    while(g_workers_ready.load(std::memory_order_acquire) < expected_workers) {
        if(riscv::csrr(riscv::registers::time) >= deadline) {
            SEELE_PANIC("ktest: timed out waiting for secondary workers ({}/{})",
                        g_workers_ready.load(std::memory_order_relaxed),
                        expected_workers);
        }
    }
    return online_target;
}

}  // namespace

Context& current() noexcept {
    return g_context;
}

void Context::barrier() noexcept {
    if(m_hart_count <= 1) {
        return;
    }
    const uint32_t generation = m_barrier_generation.load(std::memory_order_acquire);
    const size_t arrived = m_barrier_arrived.fetch_add(1, std::memory_order_acq_rel) + 1;
    if(arrived == m_hart_count) {
        m_barrier_arrived.store(0, std::memory_order_relaxed);
        m_barrier_generation.fetch_add(1, std::memory_order_release);
    } else {
        while(m_barrier_generation.load(std::memory_order_acquire) == generation) {}
    }
}

void Context::fail(const char* expr, std::source_location location) noexcept {
    m_failures.fetch_add(1, std::memory_order_acq_rel);
    println("  [FAIL] {}:{} (hart {}): expected {}",
            console::file_basename(location.file_name()),
            location.line(),
            cpuid(),
            expr);
}

void Context::fail_binary(const char* op,
                          const char* lhs_expr,
                          const char* rhs_expr,
                          format_args values,
                          std::source_location location) noexcept {
    m_failures.fetch_add(1, std::memory_order_acq_rel);
    const format_arg& lhs_val = values[0];
    const format_arg& rhs_val = values[1];
    const format_arg line_args[] = {
        format_detail::make_arg(console::file_basename(location.file_name())),
        format_detail::make_arg(location.line()),
        format_detail::make_arg(cpuid()),
        format_detail::make_arg(lhs_expr),
        format_detail::make_arg(op),
        format_detail::make_arg(rhs_expr),
        lhs_val,
        rhs_val,
    };
    vprintln("  [FAIL] {}:{} (hart {}): expected {} {} {}, got ({}) vs ({})",
             format_args{line_args, 8});
}

[[noreturn]] void worker_entry() {
    riscv::intr_on();
    const uint64_t self = cpuid();
    SEELE_ASSERT(self > 0 && self < MAX_HARTS, "ktest: invalid worker cpuid {}", self);

    worker_slot& slot = g_workers[self];
    {
        LockGuard guard{slot.lock};
        slot.thread = mycpu()->current;
    }
    g_workers_ready.fetch_add(1, std::memory_order_release);

    while(true) {
        riscv::intr_off();
        bool should_sleep = false;
        {
            LockGuard guard{slot.lock};
            if(slot.run_requested) {
                slot.run_requested = false;
            } else {
                sched::prepare_sleep();
                should_sleep = true;
            }
        }
        if(should_sleep) {
            sched::sleep();
            riscv::intr_on();
            continue;
        }
        riscv::intr_on();

        const test_case* const tc = g_active_case.load(std::memory_order_acquire);
        SEELE_ASSERT(tc != nullptr, "ktest: worker woke without active case");

        g_context.barrier();
        tc->fn();
        verify_hart_postconditions(*tc);

        g_workers_done.fetch_add(1, std::memory_order_release);
    }
}

[[noreturn]] void runner_entry() {
    riscv::intr_on();

    const size_t online_harts = wait_for_workers();
    const std::span<const test_case> cases = registered_cases();
    const std::string_view filter = parse_filter(platform::info().cmdline());

    if(filter.empty()) {
        println("[ktest] running {} registered case(s) across {} hart(s)",
                cases.size(),
                online_harts);
    } else {
        println("[ktest] running filter '{}' ({} registered) across {} hart(s)",
                filter,
                cases.size(),
                online_harts);
    }

    size_t matched = 0;
    size_t passed = 0;
    size_t failed = 0;

    for(const test_case& tc: cases) {
        if(!matches_filter(tc, filter)) {
            continue;
        }
        ++matched;

        const size_t participants = tc.is_smp ? online_harts : 1;
        g_context.reset(participants);

        if(tc.is_smp && online_harts > 1) {
            g_workers_done.store(0, std::memory_order_relaxed);
            g_active_case.store(&tc, std::memory_order_release);
            for(size_t hart = 1; hart < online_harts; ++hart) {
                wake_worker(hart);
            }
            g_context.barrier();
            tc.fn();

            const size_t expected_done = online_harts - 1;
            const uint64_t deadline = timeout_deadline();
            while(g_workers_done.load(std::memory_order_acquire) < expected_done) {
                if(riscv::csrr(riscv::registers::time) >= deadline) {
                    SEELE_PANIC("ktest: timed out waiting for workers in {}.{}", tc.suite, tc.name);
                }
            }
            g_active_case.store(nullptr, std::memory_order_release);
        } else {
            tc.fn();
        }

        verify_hart_postconditions(tc);

        if(g_context.failed()) {
            ++failed;
            println("[ FAIL ] {}.{}", tc.suite, tc.name);
        } else {
            ++passed;
            println("[ PASS ] {}.{}", tc.suite, tc.name);
        }
    }

    if(matched == 0) {
        println("[ktest] error: no test cases matched filter '{}'", filter);
        power::poweroff(1);
    }

    println("[ktest] summary: {} passed, {} failed ({} total)", passed, failed, matched);
    power::poweroff(failed == 0 ? 0 : 1);
}

}  // namespace ktest
