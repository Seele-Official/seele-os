#include <cstddef>
#include <cstdint>
#include <span>

#include "mm/addr.h"
#include "mm/heap.h"
#include <gtest/gtest.h>

namespace {

TEST(AddrTest, StrongTypesAndAlignment) {
    static_assert(address_like<Paddr>);
    static_assert(address_like<Vaddr>);
    static_assert(!address_like<uint64_t>);

    EXPECT_TRUE(is_null(null_paddr()));
    EXPECT_FALSE(is_null(paddr(0x1000)));

    EXPECT_EQ(align_down(paddr(0)), paddr(0));
    EXPECT_EQ(align_up(paddr(0)), paddr(0));
    EXPECT_EQ(align_down(paddr(0x1fff)), paddr(0x1000));
    EXPECT_EQ(align_up(paddr(0x1001)), paddr(0x2000));

    // Wrap behavior documented on align_up() past UINT64_MAX - PGOFFSET_MASK.
    EXPECT_EQ(align_up(paddr(UINT64_MAX)), paddr(0));
    EXPECT_EQ(align_down(paddr(UINT64_MAX)), paddr(UINT64_MAX & ~riscv::PGOFFSET_MASK));

    int dummy = 0;
    EXPECT_EQ(to_pointer<int>(vaddr(&dummy)), &dummy);
}

TEST(AddrTest, RangeSaturationAndOverlap) {
    EXPECT_TRUE(range_fits(vaddr(0x1000), 0));
    EXPECT_TRUE(range_fits(vaddr(UINT64_MAX - 4096), 4096));
    EXPECT_FALSE(range_fits(vaddr(UINT64_MAX - 4095), 4096));
    EXPECT_EQ(range_end(vaddr(UINT64_MAX - 10), 100), vaddr(UINT64_MAX));

    // Empty ranges overlap nothing, even inside another range.
    EXPECT_FALSE(ranges_overlap(paddr(0x2000), paddr(0x2000), paddr(0x1000), paddr(0x3000)));
    EXPECT_FALSE(ranges_overlap(paddr(0x1000), paddr(0x2000), paddr(0x2000), paddr(0x3000)));
    EXPECT_TRUE(ranges_overlap(paddr(0x1000), paddr(0x2001), paddr(0x2000), paddr(0x3000)));

    EXPECT_FALSE(page_overlaps(paddr(0x4000), paddr(0x4000), 0));
    EXPECT_TRUE(page_overlaps(paddr(0x4000), paddr(0x4fff), 1));
    EXPECT_FALSE(page_overlaps(paddr(0x4000), paddr(0x5000), 1));
}

TEST(AddrTest, NormalizeRangesEdgeCases) {
    EXPECT_EQ(normalize_ranges(std::span<phys_range>{}), size_t{0});

    // Unsorted, duplicate, nested, adjacent, and saturating-top intervals.
    phys_range ranges[] = {
        {paddr(UINT64_MAX - 0x500), 0x1000},
        {paddr(0x3000),             0x1000},
        {paddr(0x1000),             0x4000},
        {paddr(0x5000),             0x1000},
        {paddr(0x1000),             0x1000},
        {paddr(UINT64_MAX - 0x800), 0x400 },
    };

    const size_t count = normalize_ranges(std::span{ranges});
    ASSERT_EQ(count, size_t{2});
    EXPECT_EQ(ranges[0].base, paddr(0x1000));
    EXPECT_EQ(ranges[0].size, uint64_t{0x5000});
    EXPECT_EQ(ranges[1].base, paddr(UINT64_MAX - 0x800));
    EXPECT_EQ(ranges[1].size, uint64_t{0x800});
}

TEST(HeapMathTest, SizeClassesExhaustive) {
    EXPECT_EQ(heap::allocation_size(0), size_t{16});
    EXPECT_EQ(heap::class_of(0), size_t{0});

    for(size_t req = 1; req <= heap::MAX_SIZE; ++req) {
        const size_t cls = heap::class_of(req);
        ASSERT_LT(cls, heap::CLASS_COUNT);
        const size_t slot = heap::class_size(cls);
        EXPECT_GE(slot, req);
        EXPECT_EQ(slot, heap::allocation_size(req));
        if(cls > 0) {
            EXPECT_LT(heap::class_size(cls - 1), req);
        }
    }
}

}  // namespace
