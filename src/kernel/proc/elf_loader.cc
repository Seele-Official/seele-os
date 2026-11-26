#include "proc/elf_loader.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string.h>

#include "console.h"
#include "riscv.h"
#include "mm/addr.h"
#include "mm/kalloc.h"
#include "mm/vm.h"
#include "proc/elf_format.h"

namespace elf {
namespace {

[[nodiscard]] uint64_t segment_pte_perm(uint32_t flags) noexcept {
    uint64_t perm = 0;
    if((flags & PF_R) != 0) {
        perm |= riscv::PTE_R;
    }
    if((flags & PF_W) != 0) {
        perm |= riscv::PTE_W;
    }
    if((flags & PF_X) != 0) {
        perm |= riscv::PTE_X;
    }
    return perm;
}

}  // namespace

std::optional<loaded_user_image> load_user_elf(Paddr user_pt,
                                               std::span<const std::byte> image) noexcept {
    image_info info{};
    const Error err = validate(image, vm::u::USER_BASE, vm::u::USER_LOAD_LIMIT, info);
    if(err != Error::Ok) {
        LOG_ERR("elf: failed to validate user image: {}", error_name(err));
        return std::nullopt;
    }

    for(const load_segment& seg: info.load_segments()) {
        const uint64_t perm = segment_pte_perm(seg.flags);
        const Vaddr file_end = seg.va + seg.filesz;

        for(Vaddr page_va = seg.page_begin; page_va < seg.page_end; page_va += riscv::PGSIZE) {
            const Paddr page_pa = phymem::alloc();
            if(is_null(page_pa)) {
                LOG_ERR("elf: out of memory allocating user segment page");
                vm::u::destroy_user_mappings(user_pt);
                return std::nullopt;
            }

            auto* const dst = to_pointer<std::byte>(vm::k::P2V(page_pa));
            memset(dst, 0, riscv::PGSIZE);

            const Vaddr page_end = page_va + riscv::PGSIZE;
            const Vaddr copy_begin = (page_va > seg.va) ? page_va : seg.va;
            const Vaddr copy_end = (page_end < file_end) ? page_end : file_end;
            if(copy_begin < copy_end) {
                const uint64_t dst_off = copy_begin - page_va;
                const uint64_t src_off = seg.offset + (copy_begin - seg.va);
                memcpy(dst + dst_off, image.data() + src_off, copy_end - copy_begin);
            }

            if(!vm::u::map_user_pages(user_pt, page_va, riscv::PGSIZE, page_pa, perm)) {
                phymem::free(page_pa);
                LOG_ERR("elf: out of memory mapping user segment page");
                vm::u::destroy_user_mappings(user_pt);
                return std::nullopt;
            }
        }
    }

    // Allocate and map the initial user stack with an unmapped guard page below.
    for(Vaddr page_va = vm::u::USER_STACK_BASE; page_va < vm::u::USER_STACK_TOP;
        page_va += riscv::PGSIZE) {
        const Paddr page_pa = phymem::alloc();
        if(is_null(page_pa)) {
            LOG_ERR("elf: out of memory allocating user stack page");
            vm::u::destroy_user_mappings(user_pt);
            return std::nullopt;
        }
        memset(to_pointer<void>(vm::k::P2V(page_pa)), 0, riscv::PGSIZE);
        if(!vm::u::map_user_pages(user_pt,
                                  page_va,
                                  riscv::PGSIZE,
                                  page_pa,
                                  riscv::PTE_R | riscv::PTE_W)) {
            phymem::free(page_pa);
            LOG_ERR("elf: out of memory mapping user stack page");
            vm::u::destroy_user_mappings(user_pt);
            return std::nullopt;
        }
    }

    // Make freshly copied user instruction pages visible to instruction fetch.
    riscv::fence_i();

    return loaded_user_image{
        .entry = info.entry,
        .user_sp = vm::u::USER_STACK_TOP,
    };
}

}  // namespace elf
