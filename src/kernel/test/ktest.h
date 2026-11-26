#pragma once

// Minimal freestanding in-kernel test framework (`SEELE_TESTS == 1`).
//
// Test descriptors are placed in `.ktest_cases` inside `.rodata` and discovered
// at link time with zero heap allocation or `.init_array` constructors:
//   - `KTEST_CASE(suite, name)` runs on the boot hart in thread context
//     (`sstatus.SIE == 1`, no spinlocks held).
//   - `KTEST_SMP_CASE(suite, name)` runs concurrently across every online hart
//     after synchronizing all participants at a starting barrier. Multi-phase
//     SMP tests synchronize stages with `ktest::barrier()` and query participant
//     count with `ktest::hart_count()`.
//
// Standard `KTEST_EXPECT*` / `KTEST_ASSERT*` macros bind implicitly to
// `ktest::current()`, requiring no `ctx` parameter in test signatures or helper
// functions (`*_CTX` variants accept an explicit `Context&` when needed).

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <source_location>
#include <string_view>
#include <type_traits>

#include "console.h"
#include "util/format.h"

namespace ktest {

using TestFn = void (*)();

// Aligned and padded to 16 bytes (`alignof(std::max_align_t)` on rv64) so the
// compiler never inserts inter-object alignment padding inside `.ktest_cases`.
struct alignas(16) test_case {
    const char* suite;
    const char* name;
    TestFn fn;
    bool is_smp;
};

static_assert(alignof(test_case) == 16);
static_assert(sizeof(test_case) % alignof(test_case) == 0);

// Per-case execution state shared in `.bss` across all participating harts.
class Context {
public:
    constexpr Context() noexcept = default;

    explicit constexpr Context(size_t hart_count) noexcept : m_hart_count(hart_count) {}

    Context(const Context&) = delete;
    Context& operator= (const Context&) = delete;

    void reset(size_t hart_count) noexcept {
        m_hart_count = hart_count;
        m_failures.store(0, std::memory_order_relaxed);
        m_barrier_arrived.store(0, std::memory_order_relaxed);
        m_barrier_generation.store(0, std::memory_order_release);
    }

    // Number of harts participating in this test (`1` for single-hart cases,
    // online hart count for `KTEST_SMP_CASE`).
    [[nodiscard]] constexpr size_t hart_count() const noexcept {
        return m_hart_count;
    }

    [[nodiscard]] bool failed() const noexcept {
        return m_failures.load(std::memory_order_acquire) != 0;
    }

    // Reusable sense-reversing spin barrier across `hart_count()` participants.
    // No-op when `hart_count() <= 1`.
    void barrier() noexcept;

    void fail(const char* expr, std::source_location location) noexcept;

    void fail_binary(const char* op,
                     const char* lhs_expr,
                     const char* rhs_expr,
                     format_args values,
                     std::source_location location) noexcept;

private:
    size_t m_hart_count = 1;
    std::atomic<uint32_t> m_failures{0};
    std::atomic<size_t> m_barrier_arrived{0};
    std::atomic<uint32_t> m_barrier_generation{0};
};

// Active test context for the currently running case.
[[nodiscard]] Context& current() noexcept;

// Convenience wrappers around `current()` for `KTEST_SMP_CASE` bodies.
[[nodiscard]] inline size_t hart_count() noexcept {
    return current().hart_count();
}

inline void barrier() noexcept {
    current().barrier();
}

namespace detail {

template <typename T>
[[nodiscard]] constexpr auto printable(const T& value) noexcept {
    using bare = std::remove_cvref_t<T>;
    if constexpr(format_detail::is_arg_v<bare>) {
        return value;
    } else if constexpr(std::is_pointer_v<bare>) {
        return reinterpret_cast<uintptr_t>(value);
    } else if constexpr(std::is_null_pointer_v<bare>) {
        return uintptr_t{0};
    } else if constexpr(std::is_enum_v<bare>) {
        return static_cast<std::underlying_type_t<bare>>(value);
    } else {
        return std::string_view{"<unprintable>"};
    }
}

template <typename L, typename R>
[[nodiscard]] bool check_eq(Context& ctx,
                            const L& lhs,
                            const R& rhs,
                            const char* lhs_expr,
                            const char* rhs_expr,
                            std::source_location location) noexcept {
    if(lhs == rhs) {
        return true;
    }
    ctx.fail_binary("==",
                    lhs_expr,
                    rhs_expr,
                    ::make_format_args(printable(lhs), printable(rhs)),
                    location);
    return false;
}

template <typename L, typename R>
[[nodiscard]] bool check_ne(Context& ctx,
                            const L& lhs,
                            const R& rhs,
                            const char* lhs_expr,
                            const char* rhs_expr,
                            std::source_location location) noexcept {
    if(lhs != rhs) {
        return true;
    }
    ctx.fail_binary("!=",
                    lhs_expr,
                    rhs_expr,
                    ::make_format_args(printable(lhs), printable(rhs)),
                    location);
    return false;
}

}  // namespace detail

// Boot-hart test runner thread entry. Executes matching cases, prints the
// summary, and powers off the machine with exit status 0 (pass) or 1 (fail).
[[noreturn]] void runner_entry();

// Secondary-hart worker thread entry. Parks while single-hart tests run and
// joins `KTEST_SMP_CASE` runs on demand.
[[noreturn]] void worker_entry();

}  // namespace ktest

#define KTEST_DETAIL_DEFINE(suite_ident, name_ident, smp_flag)                                     \
    static void ktest_fn_##suite_ident##_##name_ident();                                           \
    static constinit const ::ktest::test_case ktest_case_##suite_ident##_##name_ident              \
        __attribute__((used, section(".ktest_cases"))) = {                                         \
            .suite = #suite_ident,                                                                 \
            .name = #name_ident,                                                                   \
            .fn = &ktest_fn_##suite_ident##_##name_ident,                                          \
            .is_smp = (smp_flag),                                                                  \
    };                                                                                             \
    static void ktest_fn_##suite_ident##_##name_ident()

#define KTEST_CASE(suite_ident, name_ident) KTEST_DETAIL_DEFINE(suite_ident, name_ident, false)
#define KTEST_SMP_CASE(suite_ident, name_ident) KTEST_DETAIL_DEFINE(suite_ident, name_ident, true)

#define KTEST_EXPECT_CTX(ctx, cond)                                                                \
    do {                                                                                           \
        if(!(cond)) {                                                                              \
            (ctx).fail(#cond, std::source_location::current());                                    \
        }                                                                                          \
    } while(false)

#define KTEST_ASSERT_CTX(ctx, cond)                                                                \
    do {                                                                                           \
        if(!(cond)) {                                                                              \
            (ctx).fail(#cond, std::source_location::current());                                    \
            return;                                                                                \
        }                                                                                          \
    } while(false)

#define KTEST_EXPECT_EQ_CTX(ctx, lhs, rhs)                                                         \
    do {                                                                                           \
        (void)::ktest::detail::check_eq((ctx),                                                     \
                                        (lhs),                                                     \
                                        (rhs),                                                     \
                                        #lhs,                                                      \
                                        #rhs,                                                      \
                                        std::source_location::current());                          \
    } while(false)

#define KTEST_ASSERT_EQ_CTX(ctx, lhs, rhs)                                                         \
    do {                                                                                           \
        if(!::ktest::detail::check_eq((ctx),                                                       \
                                      (lhs),                                                       \
                                      (rhs),                                                       \
                                      #lhs,                                                        \
                                      #rhs,                                                        \
                                      std::source_location::current())) {                          \
            return;                                                                                \
        }                                                                                          \
    } while(false)

#define KTEST_EXPECT_NE_CTX(ctx, lhs, rhs)                                                         \
    do {                                                                                           \
        (void)::ktest::detail::check_ne((ctx),                                                     \
                                        (lhs),                                                     \
                                        (rhs),                                                     \
                                        #lhs,                                                      \
                                        #rhs,                                                      \
                                        std::source_location::current());                          \
    } while(false)

#define KTEST_ASSERT_NE_CTX(ctx, lhs, rhs)                                                         \
    do {                                                                                           \
        if(!::ktest::detail::check_ne((ctx),                                                       \
                                      (lhs),                                                       \
                                      (rhs),                                                       \
                                      #lhs,                                                        \
                                      #rhs,                                                        \
                                      std::source_location::current())) {                          \
            return;                                                                                \
        }                                                                                          \
    } while(false)

#define KTEST_EXPECT(cond) KTEST_EXPECT_CTX(::ktest::current(), cond)
#define KTEST_ASSERT(cond) KTEST_ASSERT_CTX(::ktest::current(), cond)
#define KTEST_EXPECT_EQ(lhs, rhs) KTEST_EXPECT_EQ_CTX(::ktest::current(), lhs, rhs)
#define KTEST_ASSERT_EQ(lhs, rhs) KTEST_ASSERT_EQ_CTX(::ktest::current(), lhs, rhs)
#define KTEST_EXPECT_NE(lhs, rhs) KTEST_EXPECT_NE_CTX(::ktest::current(), lhs, rhs)
#define KTEST_ASSERT_NE(lhs, rhs) KTEST_ASSERT_NE_CTX(::ktest::current(), lhs, rhs)
