#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include "riscv.h"
#include "mm/addr.h"
#include "proc/elf_format.h"
#include <gtest/gtest.h>

namespace {

constexpr Vaddr TEST_MIN_VA = vaddr(0x0000'0020'0000'0000ull);
constexpr Vaddr TEST_MAX_VA = vaddr(0x0000'003f'ffff'a000ull);

struct SyntheticElf {
    std::vector<std::byte> bytes;

    elf::elf64_ehdr& ehdr() {
        return *reinterpret_cast<elf::elf64_ehdr*>(bytes.data());
    }

    elf::elf64_phdr& phdr(size_t index) {
        const uint64_t off = ehdr().e_phoff + index * ehdr().e_phentsize;
        return *reinterpret_cast<elf::elf64_phdr*>(bytes.data() + off);
    }

    [[nodiscard]] std::span<const std::byte> span() const {
        return bytes;
    }
};

SyntheticElf make_valid_elf() {
    SyntheticElf img;
    img.bytes.resize(0x3000, std::byte{0});

    elf::elf64_ehdr hdr{};
    hdr.e_ident[0] = 0x7f;
    hdr.e_ident[1] = 'E';
    hdr.e_ident[2] = 'L';
    hdr.e_ident[3] = 'F';
    hdr.e_ident[4] = elf::ELFCLASS64;
    hdr.e_ident[5] = elf::ELFDATA2LSB;
    hdr.e_ident[6] = elf::EV_CURRENT;
    hdr.e_type = elf::ET_EXEC;
    hdr.e_machine = elf::EM_RISCV;
    hdr.e_version = elf::EV_CURRENT;
    hdr.e_entry = to_uint64(TEST_MIN_VA) + 0x10000;
    hdr.e_phoff = sizeof(elf::elf64_ehdr);
    hdr.e_ehsize = sizeof(elf::elf64_ehdr);
    hdr.e_phentsize = sizeof(elf::elf64_phdr);
    hdr.e_phnum = 3;
    std::memcpy(img.bytes.data(), &hdr, sizeof(hdr));

    // Segment 0: .text (R+X)
    elf::elf64_phdr text{};
    text.p_type = elf::PT_LOAD;
    text.p_flags = elf::PF_R | elf::PF_X;
    text.p_offset = 0x1000;
    text.p_vaddr = to_uint64(TEST_MIN_VA) + 0x10000;
    text.p_filesz = 0x200;
    text.p_memsz = 0x200;
    text.p_align = riscv::PGSIZE;

    // Segment 1: .rodata (R)
    elf::elf64_phdr rodata{};
    rodata.p_type = elf::PT_LOAD;
    rodata.p_flags = elf::PF_R;
    rodata.p_offset = 0x2000;
    rodata.p_vaddr = to_uint64(TEST_MIN_VA) + 0x11000;
    rodata.p_filesz = 0x100;
    rodata.p_memsz = 0x100;
    rodata.p_align = riscv::PGSIZE;

    // Segment 2: .data + .bss (R+W, memsz > filesz)
    elf::elf64_phdr data{};
    data.p_type = elf::PT_LOAD;
    data.p_flags = elf::PF_R | elf::PF_W;
    data.p_offset = 0x2100;
    data.p_vaddr = to_uint64(TEST_MIN_VA) + 0x12000;
    data.p_filesz = 0x80;
    data.p_memsz = 0x2500;
    data.p_align = riscv::PGSIZE;

    img.phdr(0) = text;
    img.phdr(1) = rodata;
    img.phdr(2) = data;
    return img;
}

TEST(ElfFormatTest, ValidImageParsesSegments) {
    const SyntheticElf img = make_valid_elf();
    elf::image_info info{};

    ASSERT_EQ(elf::validate(img.span(), TEST_MIN_VA, TEST_MAX_VA, info), elf::Error::Ok);
    EXPECT_EQ(info.entry, TEST_MIN_VA + 0x10000);
    ASSERT_EQ(info.segment_count, size_t{3});

    EXPECT_EQ(info.segments[0].page_begin, TEST_MIN_VA + 0x10000);
    EXPECT_EQ(info.segments[0].page_end, TEST_MIN_VA + 0x11000);
    EXPECT_EQ(info.segments[0].flags, elf::PF_R | elf::PF_X);

    EXPECT_EQ(info.segments[1].page_begin, TEST_MIN_VA + 0x11000);
    EXPECT_EQ(info.segments[1].page_end, TEST_MIN_VA + 0x12000);
    EXPECT_EQ(info.segments[1].flags, elf::PF_R);

    // memsz 0x2500 rounds up to 3 pages (0x3000)
    EXPECT_EQ(info.segments[2].page_begin, TEST_MIN_VA + 0x12000);
    EXPECT_EQ(info.segments[2].page_end, TEST_MIN_VA + 0x15000);
    EXPECT_EQ(info.segments[2].filesz, uint64_t{0x80});
    EXPECT_EQ(info.segments[2].memsz, uint64_t{0x2500});
}

TEST(ElfFormatTest, RejectsMalformedHeaders) {
    elf::image_info info{};

    // Truncated buffer
    std::byte tiny[16]{};
    EXPECT_EQ(elf::validate(tiny, TEST_MIN_VA, TEST_MAX_VA, info), elf::Error::Truncated);

    // Bad magic
    {
        SyntheticElf img = make_valid_elf();
        img.ehdr().e_ident[0] = 0;
        EXPECT_EQ(elf::validate(img.span(), TEST_MIN_VA, TEST_MAX_VA, info), elf::Error::BadMagic);
    }

    // Wrong ELF class (32-bit)
    {
        SyntheticElf img = make_valid_elf();
        img.ehdr().e_ident[4] = 1;
        EXPECT_EQ(elf::validate(img.span(), TEST_MIN_VA, TEST_MAX_VA, info), elf::Error::BadClass);
    }

    // Big-endian
    {
        SyntheticElf img = make_valid_elf();
        img.ehdr().e_ident[5] = 2;
        EXPECT_EQ(elf::validate(img.span(), TEST_MIN_VA, TEST_MAX_VA, info), elf::Error::BadEndian);
    }

    // Wrong ELF type (ET_DYN instead of ET_EXEC)
    {
        SyntheticElf img = make_valid_elf();
        img.ehdr().e_type = 3;
        EXPECT_EQ(elf::validate(img.span(), TEST_MIN_VA, TEST_MAX_VA, info), elf::Error::BadType);
    }

    // Wrong machine (x86_64)
    {
        SyntheticElf img = make_valid_elf();
        img.ehdr().e_machine = 62;
        EXPECT_EQ(elf::validate(img.span(), TEST_MIN_VA, TEST_MAX_VA, info),
                  elf::Error::BadMachine);
    }

    // Program header table out of bounds / overflowing
    {
        SyntheticElf img = make_valid_elf();
        img.ehdr().e_phoff = img.bytes.size() - 8;
        EXPECT_EQ(elf::validate(img.span(), TEST_MIN_VA, TEST_MAX_VA, info),
                  elf::Error::BadPhdrTable);
    }
}

TEST(ElfFormatTest, EnforcesWXAndBoundsAndNoOverlap) {
    elf::image_info info{};

    // W+X violation
    {
        SyntheticElf img = make_valid_elf();
        img.phdr(0).p_flags = elf::PF_R | elf::PF_W | elf::PF_X;
        EXPECT_EQ(elf::validate(img.span(), TEST_MIN_VA, TEST_MAX_VA, info),
                  elf::Error::BadSegmentFlags);
    }

    // W without R violation
    {
        SyntheticElf img = make_valid_elf();
        img.phdr(2).p_flags = elf::PF_W;
        EXPECT_EQ(elf::validate(img.span(), TEST_MIN_VA, TEST_MAX_VA, info),
                  elf::Error::BadSegmentFlags);
    }

    // Segment mapped below USER_BASE (into kernel direct map)
    {
        SyntheticElf img = make_valid_elf();
        img.phdr(2).p_vaddr = 0x8000'0000ull;
        EXPECT_EQ(elf::validate(img.span(), TEST_MIN_VA, TEST_MAX_VA, info),
                  elf::Error::BadSegmentBounds);
    }

    // Segment colliding with user stack guard region (>= TEST_MAX_VA)
    {
        SyntheticElf img = make_valid_elf();
        img.phdr(2).p_vaddr = to_uint64(TEST_MAX_VA) - 0x1000;
        img.phdr(2).p_memsz = 0x2000;
        EXPECT_EQ(elf::validate(img.span(), TEST_MIN_VA, TEST_MAX_VA, info),
                  elf::Error::BadSegmentBounds);
    }

    // filesz > memsz
    {
        SyntheticElf img = make_valid_elf();
        img.phdr(1).p_filesz = 0x200;
        img.phdr(1).p_memsz = 0x100;
        EXPECT_EQ(elf::validate(img.span(), TEST_MIN_VA, TEST_MAX_VA, info),
                  elf::Error::BadSegmentBounds);
    }

    // File range past end of image
    {
        SyntheticElf img = make_valid_elf();
        img.phdr(1).p_offset = img.bytes.size() - 0x10;
        img.phdr(1).p_filesz = 0x20;
        img.phdr(1).p_memsz = 0x20;
        EXPECT_EQ(elf::validate(img.span(), TEST_MIN_VA, TEST_MAX_VA, info),
                  elf::Error::BadSegmentBounds);
    }

    // Overlapping page ranges between two PT_LOAD segments
    {
        SyntheticElf img = make_valid_elf();
        img.phdr(1).p_vaddr = img.phdr(0).p_vaddr + 0x800;
        EXPECT_EQ(elf::validate(img.span(), TEST_MIN_VA, TEST_MAX_VA, info),
                  elf::Error::SegmentOverlap);
    }

    // Entry point inside non-executable segment
    {
        SyntheticElf img = make_valid_elf();
        img.ehdr().e_entry = img.phdr(1).p_vaddr;
        EXPECT_EQ(elf::validate(img.span(), TEST_MIN_VA, TEST_MAX_VA, info), elf::Error::BadEntry);
    }

    // No PT_LOAD segments
    {
        SyntheticElf img = make_valid_elf();
        img.phdr(0).p_type = 0;
        img.phdr(1).p_type = 0;
        img.phdr(2).p_type = 0;
        EXPECT_EQ(elf::validate(img.span(), TEST_MIN_VA, TEST_MAX_VA, info),
                  elf::Error::NoLoadSegments);
    }
}

}  // namespace
