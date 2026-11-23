#include "mm/kalloc.h"

#include <cstddef>
#include <cstdint>
#include <span>

#include "console.h"
#include "panic.h"
#include "riscv.h"
#include "concurrency/lock_guard.h"
#include "concurrency/spinlock.h"
#include "mm/addr.h"
#include "mm/vm.h"

namespace phymem {
namespace {

// A free page stores the link to the next free page in its first bytes. The
// node is an aggregate whose lifetime begins implicitly in the page storage,
// and the
// page alignment of every free page satisfies the pointer's alignment, so the
// link can be written into the page itself, through the page's direct-map
// alias.
struct free_node {
    free_node* next;
};

// The free list stores Paddrs. The links are written through each page's
// direct-map alias, so a page only becomes a free-list node pointer while the
// lock is held and only inside this file.
[[nodiscard]] free_node* node_at(Paddr page) noexcept {
    return to_pointer<free_node>(vm::k::P2V(page));
}

[[nodiscard]] Paddr page_of(const free_node* node) noexcept {
    return vm::k::V2P(vaddr(node));
}

class Allocator {
public:
    // Detach the first page of the free list, or return null_paddr() when it is
    // empty. The guard releases the lock on both paths, so an empty pool cannot
    // leave the allocator locked forever.
    [[nodiscard]] Paddr alloc() noexcept {
        LockGuard guard{this->lock};
        free_node* page = this->free_list;
        if(page != nullptr) {
            this->free_list = page->next;
        }
        return page == nullptr ? null_paddr() : page_of(page);
    }

    // Push one page onto the free list. `page` must already be validated by
    // free() or produced by init().
    void push(Paddr page) noexcept {
        LockGuard guard{this->lock};
        free_node* node = node_at(page);
        node->next = this->free_list;
        this->free_list = node;
    }

private:
    free_node* free_list = nullptr;
    Spinlock lock{};
};

Allocator allocator;

// True when the page [page, page + PGSIZE) intersects [base, base + size) for
// any Reserved region. The end is saturated rather than computed, so a range
// that reaches the top of the address space cannot wrap around.
[[nodiscard]] bool overlaps_reserved(Paddr page, std::span<const boot::region> regions) noexcept {
    for(const boot::region& region: regions) {
        if(region.kind == boot::RegionKind::Reserved &&
           page_overlaps(page, region.base, region.size)) {
            return true;
        }
    }
    return false;
}

// What init() put into the pool, so free() can reject an address the pool never
// owned. push() writes the free-list link into the page itself, so freeing, say,
// an MMIO base would overwrite a device's registers instead of failing.
struct pool_range {
    Paddr first;  // page aligned, inclusive
    Paddr last;   // page aligned, exclusive
};

constexpr size_t MAX_POOL_RANGES = boot::MAX_REGIONS;

pool_range g_pool[MAX_POOL_RANGES];
size_t g_pool_count = 0;

// Reserved ranges sit inside a usable one (the DTB is inside RAM), so the pool
// ranges alone cannot tell their pages apart.
boot::region g_reserved[boot::MAX_REGIONS];
size_t g_reserved_count = 0;

Paddr g_kernel_start = null_paddr();
Paddr g_kernel_end = null_paddr();

[[nodiscard]] bool in_pool(Paddr address) noexcept {
    for(const pool_range& range: std::span{g_pool, g_pool_count}) {
        if(range_contains(range.first, range.last, address)) {
            return true;
        }
    }
    return false;
}

}  // namespace

Paddr alloc() noexcept {
    return allocator.alloc();
}

void free(Paddr page) noexcept {
    // free() cannot report an error, and silently dropping a bad page would
    // both leak it and hide the caller bug, so every violated precondition stops
    // the kernel in every build. The checks run before taking the allocator
    // lock.
    const uint64_t address = to_uint64(page);
    SEELE_ASSERT(is_page_aligned(page) && (address != 0),
                 "free: misaligned address or null page {:#x}",
                 address);

    // push() writes the free-list link into the page itself, so handing back an
    // address the pool never owned -- an MMIO base, a buffer inside the image, a
    // page the firmware reserved -- would silently overwrite it. These three
    // checks are what "the page must come from alloc()" means in practice.
    SEELE_ASSERT(in_pool(page), "free: {:#x} is not a page of the pool", address);
    SEELE_ASSERT(!range_contains(g_kernel_start, g_kernel_end, page),
                 "free: {:#x} is inside the kernel image",
                 address);
    SEELE_ASSERT(!overlaps_reserved(page, std::span{g_reserved, g_reserved_count}),
                 "free: {:#x} is inside a reserved region",
                 address);

    allocator.push(page);
}

void init(std::span<const boot::region> regions) noexcept {
    // The linker symbols are addresses in the kernel's own address space, not
    // physical addresses, while boot::region::base is physical. Converting the
    // image bounds once here is what keeps the overlap test in one address
    // space. It assumes the image is loaded at its link-time address inside the
    // direct map, which is also what map_image() relies on: PAGE_OFFSET has to
    // stay 0 until linker.ld and map_image() move together.
    g_kernel_start = vm::k::V2P(vaddr(vm::IMAGE.start));
    g_kernel_end = vm::k::V2P(vaddr(vm::IMAGE.end));

    // The boot stack pool is also in use during init(): hart 0 runs on
    // boot_stacks[0] and the secondaries on their own slices. It is skipped
    // below so alloc() cannot hand out a page a hart is still standing on, but
    // it is deliberately not added to g_reserved: each hart frees its slice
    // after switching to its idle thread's kernel stack.
    const Paddr stack_start = vm::k::V2P(vaddr(vm::BOOT_STACK.start));
    const Paddr stack_end = vm::k::V2P(vaddr(vm::BOOT_STACK.end));

    // Keep the reserved ranges: free() has to reject their pages, and it runs
    // long after the span the caller passed in is gone.
    g_reserved_count = 0;
    for(const boot::region& region: regions) {
        if(region.kind == boot::RegionKind::Reserved && g_reserved_count < boot::MAX_REGIONS) {
            g_reserved[g_reserved_count++] = region;
        }
    }

    size_t page_count = 0;
    size_t reserved_pages = 0;
    g_pool_count = 0;

    for(const boot::region& region: regions) {
        if(region.kind != boot::RegionKind::Usable || region.size == 0) {
            continue;
        }

        if(!range_fits(region.base, region.size)) {
            // The region runs past the top of the address space, so no page in
            // it can be addressed. boot::parse() drops such a region already;
            // this is the same check, for a caller that built the list itself.
            continue;
        }

        // Only whole pages can be handed out, so round the region inwards.
        const Paddr first = align_up(region.base);
        Paddr last = align_down(range_end(region.base, region.size));
        if(first < region.base) {
            // align_up() wrapped at the top of the address space.
            continue;
        }

        // The direct map is what makes a page usable: a page at or above its
        // end has no kernel alias, so only the part below it can join the pool.
        if(last > vm::k::DIRECT_MAP_END) {
            LOG_WARN("phymem: region {:#018x}+{:#018x} is past the direct map, dropping tail",
                     to_uint64(region.base),
                     region.size);
            last = vm::k::DIRECT_MAP_END;
        }
        if(first >= last) {
            continue;
        }

        // Record the range free() will accept pages from. It is the whole
        // page-aligned region; the kernel image and reserved pages are rejected
        // separately, because both live inside it.
        SEELE_ASSERT(g_pool_count < MAX_POOL_RANGES);
        g_pool[g_pool_count++] = pool_range{first, last};

        // Both ends are page-aligned here, so `page < last` implies that the
        // whole page [page, page + PGSIZE) lies inside the region.
        for(auto page = first; page < last; page += riscv::PGSIZE) {
            if(range_contains(g_kernel_start, g_kernel_end, page)) {
                continue;
            }
            // Harts are still standing on their boot stacks, so these pages stay
            // out of the pool until each hart frees its own slice after boot.
            if(range_contains(stack_start, stack_end, page)) {
                continue;
            }
            // A page that touches a reserved range in any way is dropped whole:
            // handing out the rest of a page that contains, say, the tail of
            // the DTB would break the strings that point into it.
            if(overlaps_reserved(page, std::span{g_reserved, g_reserved_count})) {
                ++reserved_pages;
                continue;
            }
            if(is_null(page)) {
                // null_paddr() is the "pool exhausted" sentinel of alloc(), so
                // physical page 0 is never handed out, whatever PAGE_OFFSET is;
                // with today's identity map it is also the null pointer alias.
                // Firmware that puts RAM at 0 loses exactly that one page.
                continue;
            }
            allocator.push(page);
            ++page_count;
        }
    }

    LOG_DEBUG("phymem: {} pages free, {} pages inside reserved regions",
              page_count,
              reserved_pages);
}

}  // namespace phymem
