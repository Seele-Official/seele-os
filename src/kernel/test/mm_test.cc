#include <cstddef>
#include <cstdint>
#include <new>
#include <span>
#include <string.h>

#include "hart.h"
#include "riscv.h"
#include "mm/addr.h"
#include "mm/heap.h"
#include "mm/kalloc.h"
#include "mm/vm.h"
#include "test/ktest.h"

namespace {

KTEST_CASE(mm, addr_ranges) {
    KTEST_EXPECT_EQ(align_down(paddr(0x8000'1234)), paddr(0x8000'1000));
    KTEST_EXPECT_EQ(align_up(paddr(0x8000'1234)), paddr(0x8000'2000));
    KTEST_EXPECT_EQ(align_up(vaddr(0x8000'1000)), vaddr(0x8000'1000));
    KTEST_EXPECT(is_page_aligned(paddr(0x8000'0000)));
    KTEST_EXPECT(!is_page_aligned(paddr(0x8000'0001)));

    // Saturating upper bound near UINT64_MAX.
    KTEST_EXPECT(!range_fits(paddr(UINT64_MAX - 15), 32));
    KTEST_EXPECT_EQ(range_end(paddr(UINT64_MAX - 15), 32), paddr(UINT64_MAX));

    KTEST_EXPECT(ranges_overlap(paddr(0x1000), paddr(0x3000), paddr(0x2000), paddr(0x4000)));
    KTEST_EXPECT(!ranges_overlap(paddr(0x1000), paddr(0x2000), paddr(0x2000), paddr(0x3000)));
    KTEST_EXPECT(page_overlaps(paddr(0x2000), paddr(0x2fff), 4));
    KTEST_EXPECT(!page_overlaps(paddr(0x2000), paddr(0x3000), 4));

    phys_range raw[] = {
        {paddr(0x5000), 0x1000},
        {paddr(0x1000), 0x2000},
        {paddr(0x2800), 0x1800},
        {paddr(0x4000), 0x0800},
    };
    const size_t merged = normalize_ranges(std::span{raw});
    KTEST_ASSERT_EQ(merged, size_t{2});
    KTEST_EXPECT_EQ(raw[0].base, paddr(0x1000));
    KTEST_EXPECT_EQ(raw[0].size, uint64_t{0x3800});
    KTEST_EXPECT_EQ(raw[1].base, paddr(0x5000));
    KTEST_EXPECT_EQ(raw[1].size, uint64_t{0x1000});
}

KTEST_CASE(mm, phymem_alloc_free) {
    constexpr size_t BATCH = 8;
    Paddr pages[BATCH]{};

    for(size_t i = 0; i < BATCH; ++i) {
        pages[i] = phymem::alloc();
        KTEST_ASSERT(!is_null(pages[i]));
        KTEST_EXPECT(is_page_aligned(pages[i]));
        for(size_t prev = 0; prev < i; ++prev) {
            KTEST_EXPECT_NE(pages[i], pages[prev]);
        }

        auto* words = to_pointer<uint64_t>(vm::k::P2V(pages[i]));
        for(size_t w = 0; w < riscv::PGSIZE / sizeof(uint64_t); ++w) {
            words[w] = (uint64_t{0xa5a5'0000u} | i) ^ w;
        }
    }

    for(size_t i = 0; i < BATCH; ++i) {
        const auto* words = to_pointer<const uint64_t>(vm::k::P2V(pages[i]));
        KTEST_EXPECT_EQ(words[0], (uint64_t{0xa5a5'0000u} | i));
        KTEST_EXPECT_EQ(words[riscv::PGSIZE / sizeof(uint64_t) - 1],
                        (uint64_t{0xa5a5'0000u} | i) ^ (riscv::PGSIZE / sizeof(uint64_t) - 1));
    }

    for(size_t i = BATCH; i > 0; --i) {
        phymem::free(pages[i - 1]);
    }

    // LIFO free list returns the most recently freed page (`pages[0]`) next.
    const Paddr reused = phymem::alloc();
    KTEST_ASSERT(!is_null(reused));
    KTEST_EXPECT_EQ(reused, pages[0]);
    phymem::free(reused);
}

KTEST_CASE(mm, heap_size_classes) {
    void* blocks[heap::CLASS_COUNT]{};

    for(size_t cls = 0; cls < heap::CLASS_COUNT; ++cls) {
        const size_t size = heap::class_size(cls);
        blocks[cls] = heap::alloc(size - 3);
        KTEST_ASSERT_NE(blocks[cls], nullptr);

        const Vaddr addr = vaddr(blocks[cls]);
        KTEST_EXPECT_EQ(to_uint64(addr) % size, uint64_t{0});
        KTEST_EXPECT(range_contains(vm::k::heap_arena(cls), vm::k::heap_arena(cls + 1), addr));
        memset(blocks[cls], static_cast<int>(0x30 + cls), size);
    }

    for(size_t cls = 0; cls < heap::CLASS_COUNT; ++cls) {
        const size_t size = heap::class_size(cls);
        const auto* bytes = static_cast<const uint8_t*>(blocks[cls]);
        KTEST_EXPECT_EQ(bytes[0], static_cast<uint8_t>(0x30 + cls));
        KTEST_EXPECT_EQ(bytes[size - 1], static_cast<uint8_t>(0x30 + cls));
        heap::free(blocks[cls]);
    }

    // Over-aligned allocation promotes into the matching size-class arena.
    void* aligned = heap::alloc(16, 256);
    KTEST_ASSERT_NE(aligned, nullptr);
    KTEST_EXPECT_EQ(reinterpret_cast<uintptr_t>(aligned) % 256, uintptr_t{0});
    KTEST_EXPECT(range_contains(vm::k::heap_arena(heap::class_of(256)),
                                vm::k::heap_arena(heap::class_of(256) + 1),
                                vaddr(aligned)));
    heap::free(aligned);
    heap::free(nullptr);

    auto* boxed = new uint64_t{0xdead'beef'cafe'babeull};
    KTEST_ASSERT_NE(boxed, nullptr);
    KTEST_EXPECT_EQ(*boxed, 0xdead'beef'cafe'babeull);
    delete boxed;
}

KTEST_CASE(mm, kstack_slots_and_pagetable) {
    const auto first = vm::k::alloc_kernel_stack();
    const auto second = vm::k::alloc_kernel_stack();
    KTEST_ASSERT(first.has_value());
    KTEST_ASSERT(second.has_value());
    KTEST_EXPECT_NE(*first, *second);
    KTEST_EXPECT(range_contains(vm::k::KSTACK_WINDOW_BASE, vm::k::KSTACK_TOP, *first));
    KTEST_EXPECT(range_contains(vm::k::KSTACK_WINDOW_BASE, vm::k::KSTACK_TOP, *second));

    // Distinct slots stay separated by at least KSTACK_SLOT_SIZE (stack + guard).
    const uint64_t distance = *first > *second ? (*first - *second) : (*second - *first);
    KTEST_EXPECT(distance >= vm::k::KSTACK_SLOT_SIZE);

    auto* first_words = to_pointer<uint64_t>(*first);
    auto* second_words = to_pointer<uint64_t>(*second);
    first_words[0] = 0x1111'2222'3333'4444ull;
    second_words[vm::k::KSTACK_SIZE / sizeof(uint64_t) - 1] = 0x5555'6666'7777'8888ull;
    KTEST_EXPECT_EQ(first_words[0], 0x1111'2222'3333'4444ull);
    KTEST_EXPECT_EQ(second_words[vm::k::KSTACK_SIZE / sizeof(uint64_t) - 1],
                    0x5555'6666'7777'8888ull);

    vm::k::free_kernel_stack(*first);
    vm::k::free_kernel_stack(*second);

    // Freed slots recycle in LIFO order without unmapping.
    const auto reused = vm::k::alloc_kernel_stack();
    KTEST_ASSERT(reused.has_value());
    KTEST_EXPECT_EQ(*reused, *second);
    vm::k::free_kernel_stack(*reused);

    // Process page tables clone frozen kernel root entries (direct map 0..127,
    // heap 256..262, and shared root-508 kstack) while leaving user entries
    // 128..255 private and zero-initialized.
    const Paddr proc_pt_pa = vm::k::create_process_pagetable();
    KTEST_ASSERT(!is_null(proc_pt_pa));
    KTEST_EXPECT_NE(proc_pt_pa, vm::k::pagetable());
    const auto* kernel_root = to_pointer<const riscv::PageTable>(vm::k::P2V(vm::k::pagetable()));
    const auto* proc_root = to_pointer<const riscv::PageTable>(vm::k::P2V(proc_pt_pa));
    KTEST_EXPECT_EQ((*proc_root)[2], (*kernel_root)[2]);
    KTEST_EXPECT_EQ((*proc_root)[128], uint64_t{0});
    KTEST_EXPECT_EQ((*proc_root)[255], uint64_t{0});
    KTEST_EXPECT_EQ((*proc_root)[256], (*kernel_root)[256]);
    KTEST_EXPECT_EQ((*proc_root)[508], (*kernel_root)[508]);
    KTEST_EXPECT(((*proc_root)[508] & riscv::PTE_V) != 0);
    phymem::free(proc_pt_pa);
}

KTEST_CASE(mm, user_memory_copy) {
    const Paddr pt = vm::k::create_process_pagetable();
    KTEST_ASSERT(!is_null(pt));

    const Paddr rw_page0 = phymem::alloc();
    const Paddr rw_page1 = phymem::alloc();
    const Paddr ro_page = phymem::alloc();
    KTEST_ASSERT(!is_null(rw_page0));
    KTEST_ASSERT(!is_null(rw_page1));
    KTEST_ASSERT(!is_null(ro_page));
    memset(to_pointer<void>(vm::k::P2V(rw_page0)), 0, riscv::PGSIZE);
    memset(to_pointer<void>(vm::k::P2V(rw_page1)), 0, riscv::PGSIZE);
    memset(to_pointer<void>(vm::k::P2V(ro_page)), 0x5a, riscv::PGSIZE);

    const Vaddr base_va = vm::u::USER_BASE + 0x10000;
    const Vaddr ro_va = vm::u::USER_BASE + 0x20000;
    KTEST_ASSERT(
        vm::u::map_user_pages(pt, base_va, riscv::PGSIZE, rw_page0, riscv::PTE_R | riscv::PTE_W));
    KTEST_ASSERT(vm::u::map_user_pages(pt,
                                       base_va + riscv::PGSIZE,
                                       riscv::PGSIZE,
                                       rw_page1,
                                       riscv::PTE_R | riscv::PTE_W));
    KTEST_ASSERT(vm::u::map_user_pages(pt, ro_va, riscv::PGSIZE, ro_page, riscv::PTE_R));

    // Cross-page copy_to_user and copy_from_user.
    char src[32]{};
    for(size_t i = 0; i < sizeof(src); ++i) {
        src[i] = static_cast<char>('A' + (i % 26));
    }
    const Vaddr cross_va = base_va + riscv::PGSIZE - 12;
    KTEST_EXPECT(vm::u::copy_to_user(pt, cross_va, src, sizeof(src)));

    char dst[32]{};
    KTEST_EXPECT(vm::u::copy_from_user(pt, dst, cross_va, sizeof(dst)));
    KTEST_EXPECT_EQ(memcmp(src, dst, sizeof(src)), 0);

    // Read-only user page allows copy_from_user but rejects copy_to_user.
    char ro_buf[8]{};
    KTEST_EXPECT(vm::u::copy_from_user(pt, ro_buf, ro_va, sizeof(ro_buf)));
    KTEST_EXPECT_EQ(ro_buf[0], static_cast<char>(0x5a));
    KTEST_EXPECT(!vm::u::copy_to_user(pt, ro_va, src, sizeof(ro_buf)));

    // Unmapped, kernel direct-map, high-half, and overflowing ranges are rejected.
    KTEST_EXPECT(!vm::u::copy_from_user(pt, dst, vm::u::USER_BASE + 0x30000, 8));
    KTEST_EXPECT(!vm::u::copy_from_user(pt, dst, vaddr(0x8000'0000ull), 8));
    KTEST_EXPECT(!vm::u::copy_from_user(pt, dst, vm::k::HEAP_BASE, 8));
    KTEST_EXPECT(!vm::u::copy_to_user(pt, vaddr(UINT64_MAX - 4), src, 16));

    // Teardown frees all user leaf pages and private page-table levels cleanly.
    vm::u::destroy_user_mappings(pt);
    KTEST_EXPECT(!vm::u::translate_user(pt, base_va, riscv::PTE_R).has_value());
    phymem::free(pt);
}

KTEST_SMP_CASE(mm, smp_allocators) {
    constexpr size_t ITERATIONS = 32;
    const uint64_t self = cpuid();

    for(size_t iter = 0; iter < ITERATIONS; ++iter) {
        const Paddr page = phymem::alloc();
        KTEST_ASSERT(!is_null(page));
        auto* page_words = to_pointer<uint64_t>(vm::k::P2V(page));
        const uint64_t tag = (self << 32) | iter | 0x5500'0000'0000'0000ull;
        page_words[0] = tag;
        page_words[511] = ~tag;

        const size_t size = heap::class_size((iter + self) % heap::CLASS_COUNT);
        auto* obj = static_cast<uint64_t*>(heap::alloc(size));
        KTEST_ASSERT_NE(obj, nullptr);
        obj[0] = tag ^ 0x1234'5678ull;
        obj[size / sizeof(uint64_t) - 1] = ~(tag ^ 0x1234'5678ull);

        KTEST_EXPECT_EQ(page_words[0], tag);
        KTEST_EXPECT_EQ(page_words[511], ~tag);
        KTEST_EXPECT_EQ(obj[0], tag ^ 0x1234'5678ull);
        KTEST_EXPECT_EQ(obj[size / sizeof(uint64_t) - 1], ~(tag ^ 0x1234'5678ull));

        heap::free(obj);
        phymem::free(page);
    }
}

}  // namespace
