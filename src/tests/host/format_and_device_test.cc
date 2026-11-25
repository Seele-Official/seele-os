#include <cstdint>
#include <string_view>

#include "console.h"
#include "device_object.h"
#include "format_check.h"
#include "format_spec.h"
#include "static_str.h"
#include <gtest/gtest.h>

namespace {

TEST(FormatTest, PlaceholderGrammar) {
    const auto plain = parse_placeholder("");
    ASSERT_TRUE(plain.has_value());
    EXPECT_FALSE(spec_has_spec(*plain));
    EXPECT_EQ(spec_base(*plain), 10u);

    const auto hex = parse_placeholder(":#018x");
    ASSERT_TRUE(hex.has_value());
    EXPECT_TRUE(spec_has_spec(*hex));
    EXPECT_TRUE(hex->alternate);
    EXPECT_TRUE(hex->zero_pad);
    EXPECT_EQ(hex->width, uint8_t{18});
    EXPECT_EQ(spec_base(*hex), 16u);
    EXPECT_FALSE(spec_uppercase(*hex));
    EXPECT_EQ(std::string_view{spec_prefix(*hex)}, "0x");

    const auto upper_bin = parse_placeholder(":#255B");
    ASSERT_TRUE(upper_bin.has_value());
    EXPECT_TRUE(spec_uppercase(*upper_bin));
    EXPECT_EQ(spec_base(*upper_bin), 2u);
    EXPECT_EQ(std::string_view{spec_prefix(*upper_bin)}, "0B");

    // Rejected placeholder bodies.
    EXPECT_FALSE(parse_placeholder(":").has_value());
    EXPECT_FALSE(parse_placeholder("08x").has_value());
    EXPECT_FALSE(parse_placeholder(":#d").has_value());
    EXPECT_FALSE(parse_placeholder(":256x").has_value());
    EXPECT_FALSE(parse_placeholder(":00001x").has_value());
    EXPECT_FALSE(parse_placeholder(":1234x").has_value());
    EXPECT_FALSE(parse_placeholder(":x8").has_value());
    EXPECT_FALSE(parse_placeholder(":p").has_value());
}

TEST(FormatTest, StaticStringAndBasename) {
    constexpr auto built =
        format_staticstr("csrrs %0, {}, {}", StaticString{"sstatus"}, StaticString{"a0"});
    static_assert(std::string_view{built} == "csrrs %0, sstatus, a0");
    EXPECT_EQ(std::string_view{built}, "csrrs %0, sstatus, a0");

    EXPECT_EQ(console::file_basename("src/kernel/mm/vm.cc"), "vm.cc");
    EXPECT_EQ(console::file_basename("vm.cc"), "vm.cc");
    EXPECT_EQ(console::file_basename("dir/"), "");
}

class AlphaInterface {
public:
    virtual int alpha() const noexcept = 0;
    uint64_t alpha_pad = 0x1111;

protected:
    ~AlphaInterface() = default;
};

class BetaInterface {
public:
    virtual int beta() const noexcept = 0;
    uint64_t beta_pad = 0x2222;

protected:
    ~BetaInterface() = default;
};

class UnimplementedInterface {
public:
    virtual void unused() noexcept = 0;

protected:
    ~UnimplementedInterface() = default;
};

class CompositeDevice final : public device::Implements<AlphaInterface, BetaInterface> {
public:
    int alpha() const noexcept override {
        return 42;
    }

    int beta() const noexcept override {
        return 99;
    }
};

TEST(DeviceObjectTest, MultipleInheritanceDynCast) {
    CompositeDevice dev{};
    device::Object* obj = &dev;
    const device::Object* const_obj = &dev;

    auto* alpha = device::dyn_cast<AlphaInterface>(obj);
    auto* beta = device::dyn_cast<BetaInterface>(obj);
    ASSERT_NE(alpha, nullptr);
    ASSERT_NE(beta, nullptr);

    // Non-zero secondary base offset must be adjusted properly (verified by
    // calling virtual methods and reading subobject fields under UBSan).
    EXPECT_NE(reinterpret_cast<void*>(alpha), reinterpret_cast<void*>(beta));
    EXPECT_EQ(alpha->alpha(), 42);
    EXPECT_EQ(beta->beta(), 99);
    EXPECT_EQ(alpha->alpha_pad, uint64_t{0x1111});
    EXPECT_EQ(beta->beta_pad, uint64_t{0x2222});

    EXPECT_EQ(device::dyn_cast<const BetaInterface>(const_obj)->beta(), 99);
    EXPECT_EQ(device::dyn_cast<UnimplementedInterface>(obj), nullptr);
    EXPECT_EQ(device::dyn_cast<AlphaInterface>(static_cast<device::Object*>(nullptr)), nullptr);
}

}  // namespace
