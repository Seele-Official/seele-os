#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>

#include "char_device.h"
#include "console.h"
#include "console_sink.h"
#include "device_object.h"
#include "util/format.h"
#include "util/static_str.h"
#include <gtest/gtest.h>

namespace {

class StringSink final : public FormatSink {
public:
    void write(std::string_view text) noexcept override {
        output.append(text.data(), text.size());
    }

    std::string output;
};

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

    const auto octal = parse_placeholder(":#o");
    ASSERT_TRUE(octal.has_value());
    EXPECT_EQ(spec_base(*octal), 8u);
    EXPECT_EQ(std::string_view{spec_prefix(*octal)}, "0");

    const auto ptr = parse_placeholder(":018p");
    ASSERT_TRUE(ptr.has_value());
    EXPECT_EQ(ptr->type, format_spec::Type::Pointer);
    EXPECT_TRUE(ptr->zero_pad);
    EXPECT_EQ(ptr->width, uint8_t{18});
    EXPECT_EQ(std::string_view{spec_prefix(*ptr)}, "0x");

    const auto upper_ptr = parse_placeholder(":P");
    ASSERT_TRUE(upper_ptr.has_value());
    EXPECT_TRUE(spec_uppercase(*upper_ptr));
    EXPECT_EQ(std::string_view{spec_prefix(*upper_ptr)}, "0X");

    const auto aligned = parse_placeholder(":*^12s");
    ASSERT_TRUE(aligned.has_value());
    EXPECT_EQ(aligned->fill, '*');
    EXPECT_EQ(aligned->align, format_spec::Align::Center);
    EXPECT_EQ(aligned->width, uint8_t{12});
    EXPECT_EQ(aligned->type, format_spec::Type::String);

    const auto signed_hex = parse_placeholder(":+#08x");
    ASSERT_TRUE(signed_hex.has_value());
    EXPECT_EQ(signed_hex->sign, format_spec::Sign::Plus);
    EXPECT_TRUE(signed_hex->alternate);
    EXPECT_TRUE(signed_hex->zero_pad);

    // Rejected placeholder bodies.
    EXPECT_FALSE(parse_placeholder(":").has_value());
    EXPECT_FALSE(parse_placeholder("08x").has_value());
    EXPECT_FALSE(parse_placeholder(":#").has_value());
    EXPECT_FALSE(parse_placeholder(":#d").has_value());
    EXPECT_FALSE(parse_placeholder(":#p").has_value());
    EXPECT_FALSE(parse_placeholder(":+s").has_value());
    EXPECT_FALSE(parse_placeholder(":+p").has_value());
    EXPECT_FALSE(parse_placeholder(":08s").has_value());
    EXPECT_FALSE(parse_placeholder(":08c").has_value());
    EXPECT_FALSE(parse_placeholder(":{<5").has_value());
    EXPECT_FALSE(parse_placeholder(":256x").has_value());
    EXPECT_FALSE(parse_placeholder(":00x").has_value());
    EXPECT_FALSE(parse_placeholder(":00001x").has_value());
    EXPECT_FALSE(parse_placeholder(":1234x").has_value());
    EXPECT_FALSE(parse_placeholder(":x8").has_value());
    EXPECT_FALSE(parse_placeholder(":q").has_value());
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

TEST(FormatTest, FormatToSink) {
    StringSink sink;
    const size_t n1 = format_to(sink,
                                "str={} bool={}/{} dec={} neg={} hex={:#06x} HEX={:#X} bin={:#08b}",
                                std::string_view{"ok"},
                                true,
                                false,
                                0,
                                -42,
                                0x1fu,
                                0xabu,
                                0b101u);
    EXPECT_EQ(n1, sink.output.size());
    EXPECT_EQ(sink.output, "str=ok bool=true/false dec=0 neg=-42 hex=0x001f HEX=0XAB bin=0b000101");

    // Signed 64-bit minimum, unsigned 64-bit maximum, padding, and multi-chunk
    // repeat (> PAD_CHUNK).
    sink.output.clear();
    const size_t n2 = format_to(sink,
                                "{}|{}|{:6d}|{:06d}|{:06x}|{:080x}",
                                std::numeric_limits<int64_t>::min(),
                                std::numeric_limits<uint64_t>::max(),
                                7,
                                -42,
                                -0x1f,
                                0x1u);
    EXPECT_EQ(n2, sink.output.size());
    EXPECT_EQ(sink.output,
              "-9223372036854775808|18446744073709551615|     7|-00042|-0001f|" +
                  std::string(79, '0') + "1");

    // Sign and alternate form together match std::format.
    sink.output.clear();
    EXPECT_EQ(format_to(sink, "{:#07x}|{:+d}|{: d}|{:+#08x}", -0x1f, 42, 42, 0x1f), size_t{24});
    EXPECT_EQ(sink.output, "-0x001f|+42| 42|+0x0001f");
}

TEST(FormatTest, CommonFormatExtensions) {
    StringSink sink;

    // Brace escaping: "{{" -> '{', "}}" -> '}', "{{{}}}" -> "{val}".
    EXPECT_EQ(format_to(sink, "{{key={{{}}}}} -> {}", 7, "ok"), size_t{15});
    EXPECT_EQ(sink.output, "{key={7}} -> ok");

    // Char formatting vs uint8_t/int8_t integer formatting.
    sink.output.clear();
    const uint8_t u8 = 65;
    const int8_t i8 = -5;
    EXPECT_EQ(format_to(sink, "{}|{:c}|{:#04x}|{}|{}", 'A', 'Z', 'A', u8, i8), size_t{14});
    EXPECT_EQ(sink.output, "A|Z|0x41|65|-5");

    // Pointers: void*, const void*, nullptr_t, {:p}, {:P}, {:018p}.
    sink.output.clear();
    void* raw_ptr = reinterpret_cast<void*>(uintptr_t{0x8020'0000});
    const void* const_ptr = reinterpret_cast<const void*>(uintptr_t{0x1a2b});
    const size_t ptr_n = format_to(sink,
                                   "{}|{:p}|{:P}|{:018p}|{:>8p}",
                                   nullptr,
                                   raw_ptr,
                                   const_ptr,
                                   raw_ptr,
                                   const_ptr);
    EXPECT_EQ(ptr_n, sink.output.size());
    EXPECT_EQ(sink.output, "0x0|0x80200000|0X1A2B|0x0000000080200000|  0x1a2b");

    // Octal including alternate form on zero and non-zero.
    sink.output.clear();
    EXPECT_EQ(format_to(sink, "{:o}|{:#o}|{:#o}", 0, 0, 10), size_t{7});
    EXPECT_EQ(sink.output, "0|0|012");

    // Fill and alignment for strings, bools, chars, and integers.
    sink.output.clear();
    const size_t align_n = format_to(sink,
                                     "{:6}|{:>6}|{:^6}|{:-^7s}|{:*>5d}|{:<5d}|{:^5}|{:>6}",
                                     "hi",
                                     "hi",
                                     "hi",
                                     "ok",
                                     42,
                                     42,
                                     'X',
                                     true);
    EXPECT_EQ(align_n, sink.output.size());
    EXPECT_EQ(sink.output, "hi    |    hi|  hi  |--ok---|***42|42   |  X  |  true");
}

TEST(FormatTest, VFormatToRuntimeEdgeCases) {
    StringSink sink;
    EXPECT_EQ(vformat_to(sink, "", format_args{}), size_t{0});
    EXPECT_EQ(sink.output, "");

    // Lone right brace, unclosed left brace, and invalid placeholder body with
    // no arguments are emitted literally.
    EXPECT_EQ(vformat_to(sink, "abc } def {g", format_args{}), size_t{12});
    EXPECT_EQ(sink.output, "abc } def {g");
    sink.output.clear();
    EXPECT_EQ(vformat_to(sink, "abc {def }g", format_args{}), size_t{11});
    EXPECT_EQ(sink.output, "abc {def }g");

    // Escaped braces in runtime strings.
    sink.output.clear();
    EXPECT_EQ(vformat_to(sink, "{{{}}}", make_format_args("x")), size_t{3});
    EXPECT_EQ(sink.output, "{x}");

    // Malformed placeholder bodies are preserved literally when an arg exists.
    sink.output.clear();
    EXPECT_EQ(vformat_to(sink, "a {:z} b {foo} c {}", make_format_args(1, 2, 3)), size_t{18});
    EXPECT_EQ(sink.output, "a {:z} b {foo} c 3");

    // Missing arguments emit nothing for trailing placeholders; extra arguments
    // are ignored.
    sink.output.clear();
    EXPECT_EQ(vformat_to(sink, "[{}][{}][{}]", make_format_args("only")), size_t{10});
    EXPECT_EQ(sink.output, "[only][][]");

    sink.output.clear();
    EXPECT_EQ(vformat_to(sink, "{}", make_format_args(10, 20)), size_t{2});
    EXPECT_EQ(sink.output, "10");
}

TEST(FormatTest, FormatToSpanBuffer) {
    // Larger buffer: exact written count, trailing sentinel bytes untouched.
    std::array<char, 32> buf{};
    buf.fill('\x7f');
    const size_t n = format_to(buf, "hart {} @ {:#x}", 3, 0x8020'0000ull);
    ASSERT_EQ(n, size_t{19});
    EXPECT_EQ(std::string_view(buf.data(), n), "hart 3 @ 0x80200000");
    EXPECT_EQ(buf[n], '\x7f');

    // Exact fit.
    char exact[5]{};
    const size_t exact_n = format_to(exact, "hello");
    EXPECT_EQ(exact_n, size_t{5});
    EXPECT_EQ(std::string_view(exact, exact_n), "hello");

    // Truncation clamps to span size without overflowing.
    char small[8]{};
    const size_t small_n = format_to(small, "hello world {}", 12345);
    EXPECT_EQ(small_n, size_t{8});
    EXPECT_EQ(std::string_view(small, small_n), "hello wo");

    // Empty span and runtime vformat_to(span, ...) overload.
    EXPECT_EQ(format_to(std::span<char>{}, "dropped {}", 42), size_t{0});
    char runtime_buf[6]{};
    const size_t runtime_n =
        vformat_to(std::span<char>{runtime_buf}, "v={}", make_format_args(999999));
    EXPECT_EQ(runtime_n, size_t{6});
    EXPECT_EQ(std::string_view(runtime_buf, runtime_n), "v=9999");
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

class SinkOnlyDevice final : public device::Implements<ConsoleSink> {
public:
    void write_sync(std::string_view data) noexcept override {
        sync_out.append(data.data(), data.size());
    }

    void flush() noexcept override {
        ++flush_count;
    }

    void write_emergency(std::string_view data) noexcept override {
        emergency_out.append(data.data(), data.size());
    }

    void enter_panic() noexcept override {
        panicked = true;
    }

    std::string sync_out;
    std::string emergency_out;
    int flush_count = 0;
    bool panicked = false;
};

class CharOnlyDevice final : public device::Implements<CharDevice> {
public:
    [[nodiscard]] std::size_t read(std::span<char> buffer) noexcept override {
        if(buffer.empty()) {
            return 0;
        }
        buffer[0] = 'z';
        return 1;
    }

    [[nodiscard]] std::size_t write(std::span<const char> data) noexcept override {
        stream_out.append(data.data(), data.size());
        return data.size();
    }

    std::string stream_out;
};

class DualConsoleDevice final : public device::Implements<ConsoleSink, CharDevice> {
public:
    void write_sync(std::string_view data) noexcept override {
        sync_out.append(data.data(), data.size());
    }

    void flush() noexcept override {}

    void write_emergency(std::string_view data) noexcept override {
        emergency_out.append(data.data(), data.size());
    }

    void enter_panic() noexcept override {}

    [[nodiscard]] std::size_t read(std::span<char> buffer) noexcept override {
        if(buffer.empty()) {
            return 0;
        }
        buffer[0] = 'x';
        return 1;
    }

    [[nodiscard]] std::size_t write(std::span<const char> data) noexcept override {
        stream_out.append(data.data(), data.size());
        return data.size();
    }

    std::string sync_out;
    std::string emergency_out;
    std::string stream_out;
};

TEST(DeviceObjectTest, ConsoleSinkAndCharDeviceOrthogonal) {
    SinkOnlyDevice sink_only{};
    CharOnlyDevice char_only{};
    DualConsoleDevice dual{};

    EXPECT_NE(device::dyn_cast<ConsoleSink>(&sink_only), nullptr);
    EXPECT_EQ(device::dyn_cast<CharDevice>(&sink_only), nullptr);

    EXPECT_EQ(device::dyn_cast<ConsoleSink>(&char_only), nullptr);
    EXPECT_NE(device::dyn_cast<CharDevice>(&char_only), nullptr);

    auto* sink = device::dyn_cast<ConsoleSink>(&dual);
    auto* chars = device::dyn_cast<CharDevice>(&dual);
    ASSERT_NE(sink, nullptr);
    ASSERT_NE(chars, nullptr);
    EXPECT_NE(reinterpret_cast<void*>(sink), reinterpret_cast<void*>(chars));

    sink->write_sync("log");
    sink->write_emergency("panic");
    const char payload[] = {'u', 's', 'e', 'r'};
    EXPECT_EQ(chars->write(payload), std::size_t{4});
    char rx = '\0';
    EXPECT_EQ(chars->read(std::span<char>{&rx, 1}), std::size_t{1});

    EXPECT_EQ(dual.sync_out, "log");
    EXPECT_EQ(dual.emergency_out, "panic");
    EXPECT_EQ(dual.stream_out, "user");
    EXPECT_EQ(rx, 'x');
}

}  // namespace
