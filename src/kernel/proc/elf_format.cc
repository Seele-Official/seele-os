#include "proc/elf_format.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string.h>

#include "riscv.h"
#include "mm/addr.h"

namespace elf {
namespace {

[[nodiscard]] bool valid_segment_flags(uint32_t flags) noexcept {
    const bool r = (flags & PF_R) != 0;
    const bool w = (flags & PF_W) != 0;
    const bool x = (flags & PF_X) != 0;
    if(!r && !w && !x) {
        return false;
    }
    // Sv39 forbids W without R, and kernel policy enforces W^X.
    if(w && (!r || x)) {
        return false;
    }
    return true;
}

}  // namespace

Error validate(std::span<const std::byte> image,
               Vaddr min_va,
               Vaddr max_va,
               image_info& out) noexcept {
    out = image_info{};

    if(image.size() < sizeof(elf64_ehdr)) {
        return Error::Truncated;
    }

    elf64_ehdr ehdr{};
    memcpy(&ehdr, image.data(), sizeof(ehdr));

    if(ehdr.e_ident[0] != 0x7f || ehdr.e_ident[1] != 'E' || ehdr.e_ident[2] != 'L' ||
       ehdr.e_ident[3] != 'F') {
        return Error::BadMagic;
    }
    if(ehdr.e_ident[4] != ELFCLASS64) {
        return Error::BadClass;
    }
    if(ehdr.e_ident[5] != ELFDATA2LSB) {
        return Error::BadEndian;
    }
    if(ehdr.e_ident[6] != EV_CURRENT || ehdr.e_version != EV_CURRENT) {
        return Error::BadVersion;
    }
    if(ehdr.e_type != ET_EXEC) {
        return Error::BadType;
    }
    if(ehdr.e_machine != EM_RISCV) {
        return Error::BadMachine;
    }

    if(ehdr.e_phnum == 0 || ehdr.e_phentsize < sizeof(elf64_phdr)) {
        return Error::BadPhdrTable;
    }
    if(ehdr.e_phoff > image.size() ||
       static_cast<uint64_t>(ehdr.e_phnum) > (image.size() - ehdr.e_phoff) / ehdr.e_phentsize) {
        return Error::BadPhdrTable;
    }

    if(!is_page_aligned(min_va) || !is_page_aligned(max_va) || min_va >= max_va) {
        return Error::BadSegmentBounds;
    }

    const Vaddr entry = vaddr(ehdr.e_entry);
    bool entry_in_exec_segment = false;

    for(uint16_t i = 0; i < ehdr.e_phnum; ++i) {
        const uint64_t phdr_off = ehdr.e_phoff + static_cast<uint64_t>(i) * ehdr.e_phentsize;
        elf64_phdr phdr{};
        memcpy(&phdr, image.data() + phdr_off, sizeof(phdr));

        if(phdr.p_type != PT_LOAD) {
            continue;
        }
        if(phdr.p_filesz > phdr.p_memsz) {
            return Error::BadSegmentBounds;
        }
        if(phdr.p_memsz == 0) {
            continue;
        }
        if(phdr.p_offset > image.size() || phdr.p_filesz > image.size() - phdr.p_offset) {
            return Error::BadSegmentBounds;
        }
        if(!valid_segment_flags(phdr.p_flags)) {
            return Error::BadSegmentFlags;
        }

        const Vaddr seg_va = vaddr(phdr.p_vaddr);
        if(!range_fits(seg_va, phdr.p_memsz)) {
            return Error::BadSegmentBounds;
        }
        const Vaddr seg_end = seg_va + phdr.p_memsz;
        if(to_uint64(seg_end) > UINT64_MAX - riscv::PGOFFSET_MASK) {
            return Error::BadSegmentBounds;
        }

        const Vaddr page_begin = align_down(seg_va);
        const Vaddr page_end = align_up(seg_end);
        if(page_begin < min_va || page_end > max_va) {
            return Error::BadSegmentBounds;
        }

        for(size_t prev = 0; prev < out.segment_count; ++prev) {
            if(ranges_overlap(page_begin,
                              page_end,
                              out.segments[prev].page_begin,
                              out.segments[prev].page_end)) {
                return Error::SegmentOverlap;
            }
        }

        if(out.segment_count == MAX_LOAD_SEGMENTS) {
            return Error::TooManySegments;
        }

        out.segments[out.segment_count++] = load_segment{
            .va = seg_va,
            .page_begin = page_begin,
            .page_end = page_end,
            .offset = phdr.p_offset,
            .filesz = phdr.p_filesz,
            .memsz = phdr.p_memsz,
            .flags = phdr.p_flags,
        };

        if((phdr.p_flags & PF_X) != 0 && range_contains(seg_va, seg_end, entry)) {
            entry_in_exec_segment = true;
        }
    }

    if(out.segment_count == 0) {
        return Error::NoLoadSegments;
    }
    if(!entry_in_exec_segment) {
        return Error::BadEntry;
    }

    out.entry = entry;
    return Error::Ok;
}

}  // namespace elf
