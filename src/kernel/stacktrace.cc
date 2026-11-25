#include "stacktrace.h"

#include <cstddef>
#include <cstdint>

#include "console.h"
#include "hart.h"
#include "boot/stack.h"
#include "mm/vm.h"

namespace {

// One link of the frame-pointer chain. Every frame stores the two registers it
// may clobber at a fixed distance below its own s0 --
//
//     [s0-8]   the return address into the caller
//     [s0-16]  the caller's s0
//
// -- independent of the frame size, because GCC always places the
// callee-saved registers at the top of the frame. The addresses are read as
// words, so the walker does not need to know how big the frame is.
struct link {
    uintptr_t ra;
    uintptr_t fp;
};

// True when `fp` is a usable frame pointer for the stack [bottom, top). `top`
// itself is allowed: it is the stack top, and the first frame on a stack (the
// one switch_thread() jumped to) has its frame pointer near there. Both words of
// a link are read from below fp, so the lowest frame pointer that can be
// dereferenced is bottom + 16. sp is 16-byte aligned (the psABI guarantees it,
// and every prologue subtracts a multiple of 16), so an unaligned frame pointer
// is already a sign of a corrupted chain.
//
// The bounds are a Vaddr because a stack is an address range the kernel built;
// `fp` is a uintptr_t because it is a value read back out of a frame record,
// which is a register word before it is an address. The two meet in the one
// comparison below.
[[nodiscard]] bool within_stack(uintptr_t fp, Vaddr bottom, Vaddr top) noexcept {
    const uint64_t lowest = to_uint64(bottom + 16);
    return fp % 16 == 0 && fp >= lowest && fp <= to_uint64(top);
}

// The range the chain has to stay inside: the stack of the thread doing the
// unwind. A corrupted frame pointer from one thread must not be accepted just
// because it lands in a different thread's stack.
//
// Once mycpu()->current names a thread, that thread's thread::kstack is the
// range: every thread has its own slot in the shared kernel-stack window
// (mm/vm.h), and switch_thread() keeps the walk on the running thread's stack.
// The boot stack is no longer accepted then, because idle() returns its pages
// to phymem (free_boot_stack()); after that they can be handed out as page
// tables or heap pages, and following a stale frame pointer into them would
// read memory that is no longer a stack. Before the first thread exists, the
// hart is still on its boot stack, named by the linker layout, so only that
// case falls back to the boot stack range.
[[nodiscard]] bool in_stack(uintptr_t fp) noexcept {
    const cpu* hart = mycpu();
    if(hart->current != nullptr) {
        const proc::kernel_stack& stack = hart->current->kstack;
        return within_stack(fp, stack.base, range_end(stack.base, stack.size));
    }

    const uint64_t index = cpuid();
    if(index >= boot::STACK_COUNT) {
        return false;
    }

    const Vaddr bottom = vaddr(vm::BOOT_STACK.start) + boot::stack_offset(index);
    return within_stack(fp, bottom, range_end(bottom, boot::stack_size(index)));
}

[[nodiscard]] link read_link(uintptr_t fp) noexcept {
    return link{*reinterpret_cast<const uintptr_t*>(fp - 8),
                *reinterpret_cast<const uintptr_t*>(fp - 16)};
}

}  // namespace

void stacktrace::print() noexcept {
    // This function's own frame pointer is the caller's sp, so the first link
    // read here already belongs to the caller: the walker's frame is skipped
    // without a separate "skip N frames" count.
    uintptr_t fp = reinterpret_cast<uintptr_t>(__builtin_frame_address(0));

    for(size_t depth = 0; depth < MAX_DEPTH; ++depth) {
        if(!in_stack(fp)) {
            return;
        }

        const link frame = read_link(fp);
        if(frame.ra == 0) {
            // No return address: the outermost caller was entered without one
            // (start.S reaches start() with a tail jump).
            return;
        }

        console::println_emergency("  #{} {:#018x}", depth, frame.ra);

        // The stack grows down, so a caller's frame pointer is strictly above
        // the current one. Anything else is the end of the chain: _entry never
        // set s0, so the outermost saved s0 is 0.
        if(frame.fp <= fp) {
            return;
        }
        fp = frame.fp;
    }

    console::println_emergency("  ... truncated at {} frames", MAX_DEPTH);
}
