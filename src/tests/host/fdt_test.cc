#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <span>
#include <string_view>
#include <vector>
#include <libfdt.h>

#include "fdt/fdt.h"
#include <gtest/gtest.h>

namespace {

void write_be32(std::byte* dst, uint32_t val) {
    const fdt32_t be = cpu_to_fdt32(val);
    std::memcpy(dst, &be, sizeof(be));
}

[[nodiscard]] int prop_cells(void* fdt, const char* name, std::initializer_list<uint32_t> cells) {
    std::vector<fdt32_t> be;
    be.reserve(cells.size());
    for(uint32_t cell: cells) {
        be.push_back(cpu_to_fdt32(cell));
    }
    return fdt_property(fdt, name, be.data(), static_cast<int>(be.size() * sizeof(fdt32_t)));
}

[[nodiscard]] int prop_strings(void* fdt,
                               const char* name,
                               std::initializer_list<std::string_view> values) {
    std::vector<char> buf;
    for(std::string_view s: values) {
        buf.insert(buf.end(), s.begin(), s.end());
        buf.push_back('\0');
    }
    return fdt_property(fdt, name, buf.data(), static_cast<int>(buf.size()));
}

[[nodiscard]] std::vector<std::byte> make_sample_dtb() {
    std::vector<std::byte> buf(4096);
    void* fdt = buf.data();
    const auto check = [](int err) {
        EXPECT_EQ(err, 0) << fdt_strerror(err);
    };

    check(fdt_create(fdt, static_cast<int>(buf.size())));
    check(fdt_add_reservemap_entry(fdt, 0x8000'0000, 0x2000));
    check(fdt_finish_reservemap(fdt));

    check(fdt_begin_node(fdt, ""));
    check(fdt_property_u32(fdt, "#address-cells", 2));
    check(fdt_property_u32(fdt, "#size-cells", 2));
    // Overwritten with FDT_NOP tokens after fdt_finish() to exercise NOP skipping.
    check(fdt_property_u32(fdt, "seele,nop-placeholder", 0));

    check(fdt_begin_node(fdt, "cpus"));
    check(fdt_property_u32(fdt, "#address-cells", 1));
    check(fdt_property_u32(fdt, "#size-cells", 0));
    check(fdt_property_u32(fdt, "timebase-frequency", 10'000'000));
    check(fdt_begin_node(fdt, "cpu@0"));
    check(prop_cells(fdt, "reg", {0}));
    check(fdt_property_string(fdt, "status", "okay"));
    check(fdt_end_node(fdt));
    check(fdt_begin_node(fdt, "cpu@1"));
    check(prop_cells(fdt, "reg", {1}));
    check(fdt_property_string(fdt, "status", "disabled"));
    check(fdt_end_node(fdt));
    check(fdt_end_node(fdt));  // cpus

    check(fdt_begin_node(fdt, "soc"));
    check(fdt_property_u32(fdt, "#address-cells", 2));
    check(fdt_property_u32(fdt, "#size-cells", 2));
    check(prop_strings(fdt, "compatible", {"simple-bus"}));

    check(fdt_begin_node(fdt, "plic@c000000"));
    check(fdt_property_u32(fdt, "phandle", 0x0a));
    check(prop_strings(fdt, "compatible", {"sifive,plic-1.0.0", "riscv,plic0"}));
    check(prop_cells(fdt, "reg", {0, 0x0c00'0000, 0, 0x0040'0000}));
    check(fdt_end_node(fdt));

    check(fdt_begin_node(fdt, "serial@10000000"));
    check(fdt_property_u32(fdt, "linux,phandle", 0x2b));
    check(prop_strings(fdt, "compatible", {"ns16550a", "ns16550"}));
    // Include a zero-length reg entry to verify RegRange skips it.
    check(prop_cells(fdt, "reg", {0, 0x1000'0000, 0, 0, 0, 0x1000'0000, 0, 0x100}));
    check(fdt_end_node(fdt));

    check(fdt_end_node(fdt));  // soc
    check(fdt_end_node(fdt));  // root
    check(fdt_finish(fdt));
    check(fdt_nop_property(fdt, 0, "seele,nop-placeholder"));

    buf.resize(fdt_totalsize(fdt));
    return buf;
}

TEST(FdtTest, ValidQueriesAndUnalignedBlob) {
    const std::vector<std::byte> aligned = make_sample_dtb();
    // Shift by 1 byte in a heap buffer so UBSan verifies unaligned big-endian
    // reads never invoke misaligned pointer UB.
    std::vector<std::byte> unaligned(aligned.size() + 1, std::byte{0xff});
    std::memcpy(unaligned.data() + 1, aligned.data(), aligned.size());

    const fdt::Tree tree{unaligned.data() + 1};
    ASSERT_TRUE(tree.valid());
    EXPECT_EQ(tree.size(), static_cast<uint32_t>(aligned.size()));

    std::vector<phys_range> rsv;
    for(const phys_range r: tree.reserved_regions()) {
        rsv.push_back(r);
    }
    ASSERT_EQ(rsv.size(), size_t{1});
    EXPECT_EQ(rsv[0].base, paddr(0x8000'0000));
    EXPECT_EQ(rsv[0].size, uint64_t{0x2000});

    const fdt::Node cpu0 = tree.find("/cpus/cpu@0");
    const fdt::Node cpu1 = tree.find("/cpus/cpu@1");
    ASSERT_TRUE(cpu0.valid());
    ASSERT_TRUE(cpu1.valid());
    EXPECT_TRUE(cpu0.enabled());
    EXPECT_FALSE(cpu1.enabled());
    EXPECT_EQ(cpu0.basename(), "cpu");
    EXPECT_EQ(cpu0.reg_address().value_or(99), uint64_t{0});
    EXPECT_EQ(cpu1.reg_address().value_or(99), uint64_t{1});
    EXPECT_EQ(cpu0.parent(), tree.find("/cpus"));

    const fdt::Node uart = tree.find("/soc/serial");
    ASSERT_TRUE(uart.valid());
    EXPECT_EQ(uart.name(), "serial@10000000");
    EXPECT_TRUE(uart.is_compatible("ns16550a"));
    EXPECT_TRUE(uart.is_compatible("ns16550"));
    EXPECT_FALSE(uart.is_compatible("virtio,mmio"));

    EXPECT_EQ(tree.by_phandle(0x0a), tree.find("/soc/plic"));
    EXPECT_EQ(tree.by_phandle(0x2b), uart);
    EXPECT_FALSE(tree.by_phandle(0x99).valid());

    std::vector<phys_range> regs;
    for(const phys_range r: uart.regs()) {
        regs.push_back(r);
    }
    ASSERT_EQ(regs.size(), size_t{1});
    EXPECT_EQ(regs[0].base, paddr(0x1000'0000));
    EXPECT_EQ(regs[0].size, uint64_t{0x100});

    size_t visited_without_cpus = 0;
    tree.walk([&](fdt::Node node) noexcept {
        ++visited_without_cpus;
        if(node.name() == "cpus") {
            return fdt::WalkAction::SkipChildren;
        }
        return fdt::WalkAction::Continue;
    });
    EXPECT_EQ(visited_without_cpus, size_t{5});
}

TEST(FdtTest, MaxDepthTruncation) {
    std::vector<std::byte> blob(4096 + (size_t{fdt::MAX_DEPTH} + 4) * 32);
    void* fdt = blob.data();

    ASSERT_EQ(fdt_create(fdt, static_cast<int>(blob.size())), 0);
    ASSERT_EQ(fdt_finish_reservemap(fdt), 0);
    ASSERT_EQ(fdt_begin_node(fdt, ""), 0);
    for(uint32_t d = 0; d < fdt::MAX_DEPTH + 3; ++d) {
        ASSERT_EQ(fdt_begin_node(fdt, "n"), 0);
    }
    for(uint32_t d = 0; d < fdt::MAX_DEPTH + 3; ++d) {
        ASSERT_EQ(fdt_end_node(fdt), 0);
    }
    ASSERT_EQ(fdt_end_node(fdt), 0);
    ASSERT_EQ(fdt_finish(fdt), 0);
    blob.resize(fdt_totalsize(fdt));

    const fdt::Tree tree{blob.data()};
    ASSERT_TRUE(tree.valid());

    bool truncated = false;
    size_t visited = 0;
    tree.walk(
        [&](fdt::Node) noexcept {
            ++visited;
            return fdt::WalkAction::Continue;
        },
        &truncated);
    EXPECT_TRUE(truncated);
    EXPECT_EQ(visited, size_t{fdt::MAX_DEPTH + 1});
}

void exercise_tree_queries(const std::byte* data, size_t size) {
    std::vector<std::byte> tight(data, data + size);
    const fdt::Tree tree{tight.empty() ? nullptr : tight.data()};
    (void)tree.valid();
    for(const phys_range r: tree.reserved_regions()) {
        (void)r;
    }
    (void)tree.find("/cpus/cpu@0");
    (void)tree.find("/soc/serial");
    (void)tree.by_phandle(0x0a);

    bool truncated = false;
    tree.walk(
        [](fdt::Node node, std::span<const fdt::Node> ancestors) noexcept {
            (void)node.name();
            (void)node.basename();
            (void)node.enabled();
            (void)node.is_compatible("ns16550a");
            (void)node.reg_address();
            for(const phys_range r: node.regs(ancestors)) {
                (void)r;
            }
            return fdt::WalkAction::Continue;
        },
        &truncated);
}

TEST(FdtTest, MalformedHeadersAndTruncationSweepUnderAsan) {
    EXPECT_FALSE(fdt::Tree{nullptr}.valid());

    const std::vector<std::byte> valid_blob = make_sample_dtb();

    for(size_t cut = 8; cut < valid_blob.size(); ++cut) {
        std::vector<std::byte> truncated(valid_blob.begin(),
                                         valid_blob.begin() + static_cast<ptrdiff_t>(cut));
        write_be32(truncated.data() + 4, static_cast<uint32_t>(truncated.size()));
        exercise_tree_queries(truncated.data(), truncated.size());
    }

    for(size_t offset = 0; offset + 4 <= valid_blob.size(); offset += 4) {
        for(uint32_t poison: {0u, 0xffff'ffffu, 0x7fff'fff0u, 18u}) {
            std::vector<std::byte> mutated = valid_blob;
            write_be32(mutated.data() + offset, poison);
            if(offset == 4 && poison > mutated.size()) {
                continue;
            }
            exercise_tree_queries(mutated.data(), mutated.size());
        }
    }

    std::vector<std::byte> no_nuls = valid_blob;
    for(size_t i = 40; i < no_nuls.size(); ++i) {
        if(no_nuls[i] == std::byte{0}) {
            no_nuls[i] = std::byte{'X'};
        }
    }
    exercise_tree_queries(no_nuls.data(), no_nuls.size());
}

}  // namespace
