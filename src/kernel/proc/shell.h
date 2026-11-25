#pragma once

// Interactive kernel thread in the system process (`proc::kernel_proc()`),
// created only when a readable console is attached. It shares the kernel
// address space and has its own guarded kernel stack slot.
//
// Console input waits in the device driver. An interrupt-driven UART can park
// the shell until input arrives; a polling device uses its own wait path.
// This is a kernel diagnostic interface, not a user process or program loader.

namespace proc {

// Entry point of the kernel shell thread. Never returns: it is a scheduling
// unit, so an exit would need a state the kernel does not have yet.
[[noreturn]] void shell();

}  // namespace proc
