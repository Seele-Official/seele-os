#pragma once

// U-mode system call dispatcher.
//
// Invoked from `trap::usertrap()` on `U_ECALL` after `frame->sepc` has been
// advanced past the 4-byte `ecall` instruction and S-mode interrupts have been
// re-enabled (so blocking calls such as `read()` can sleep on UART input).

#include "trap.h"

namespace syscall {

// Dispatch the system call encoded in `frame` (`a7` = syscall number, `a0..a5`
// = arguments) and store the signed result in `frame->a0`. Non-returning calls
// (`exit`, successful `shutdown`) do not return to the caller.
void dispatch(trap::trap_frame* frame) noexcept;

}  // namespace syscall
