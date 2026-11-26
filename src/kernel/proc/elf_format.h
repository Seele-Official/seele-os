#pragma once

// Hardware-independent ELF64 RISC-V executable parser and validator.
//
// Validates static ET_EXEC ELF64 images against strict W^X, bounds, alignment,
// and non-overlap rules without touching page tables or physical memory, so the
// same code runs in both the kernel loader and host-side ASan/UBSan tests.

#include <cstddef>
#include <cstdint>
#include <span>

#include "mm/addr.h"

namespace elf {

constexpr uint8_t ELFCLASS64 = 2;
constexpr uint8_t ELFDATA2LSB = 1;
constexpr uint8_t EV_CURRENT = 1;
constexpr uint16_t ET_EXEC = 2;
constexpr uint16_t EM_RISCV = 243;

constexpr uint32_t PT_LOAD = 1;

constexpr uint32_t PF_X = 0x1;
constexpr uint32_t PF_W = 0x2;
constexpr uint32_t PF_R = 0x4;

struct elf64_ehdr {
    uint8_t e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

struct elf64_phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
};

static_assert(sizeof(elf64_ehdr) == 64);
static_assert(sizeof(elf64_phdr) == 56);

enum class Error : uint8_t {
    Ok = 0,
    Truncated,
    BadMagic,
    BadClass,
    BadEndian,
    BadVersion,
    BadType,
    BadMachine,
    BadPhdrTable,
    NoLoadSegments,
    TooManySegments,
    BadSegmentBounds,
    BadSegmentFlags,
    SegmentOverlap,
    BadEntry,
};

[[nodiscard]] constexpr const char* error_name(Error err) noexcept {
    switch(err) {
        case Error::Ok: return "ok";
        case Error::Truncated: return "truncated header";
        case Error::BadMagic: return "invalid ELF magic";
        case Error::BadClass: return "not ELF64";
        case Error::BadEndian: return "not little-endian";
        case Error::BadVersion: return "unsupported ELF version";
        case Error::BadType: return "not ET_EXEC";
        case Error::BadMachine: return "not EM_RISCV";
        case Error::BadPhdrTable: return "invalid program header table";
        case Error::NoLoadSegments: return "no PT_LOAD segments";
        case Error::TooManySegments: return "too many PT_LOAD segments";
        case Error::BadSegmentBounds: return "segment out of bounds or overflow";
        case Error::BadSegmentFlags: return "invalid or non-W^X segment permissions";
        case Error::SegmentOverlap: return "overlapping PT_LOAD page ranges";
        case Error::BadEntry: return "entry point outside executable segment";
    }
    return "unknown";
}

constexpr size_t MAX_LOAD_SEGMENTS = 16;

// One validated, non-empty PT_LOAD segment. `[page_begin, page_end)` is the
// 4 KiB page-rounded virtual range to map; file bytes [offset, offset + filesz)
// map starting at `va`, and [va + filesz, va + memsz) is zero-filled.
struct load_segment {
    Vaddr va{};
    Vaddr page_begin{};
    Vaddr page_end{};
    uint64_t offset = 0;
    uint64_t filesz = 0;
    uint64_t memsz = 0;
    uint32_t flags = 0;
};

struct image_info {
    Vaddr entry{};
    load_segment segments[MAX_LOAD_SEGMENTS]{};
    size_t segment_count = 0;

    [[nodiscard]] constexpr std::span<const load_segment> load_segments() const noexcept {
        return {this->segments, this->segment_count};
    }
};

// Parse and validate an ELF64 RISC-V executable against allowed virtual range
// `[min_va, max_va)` (both must be page aligned with `min_va < max_va`).
// On `Error::Ok`, `out` receives the entry point and validated PT_LOAD list.
[[nodiscard]] Error validate(std::span<const std::byte> image,
                             Vaddr min_va,
                             Vaddr max_va,
                             image_info& out) noexcept;

}  // namespace elf
