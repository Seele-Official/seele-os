#pragma once

// Interactive kernel thread in the boot hart's system process, created only
// when a console is attached. It shares the system address space and has its
// own kernel stack, just like any other kernel thread.
//
// Console input waits in the device driver. An interrupt-driven UART can park
// the shell until input arrives; a polling device uses its own wait path.
// This is a kernel diagnostic interface, not a user process or program loader.

namespace proc {

// Entry point of the kernel shell thread. Never returns: it is a scheduling
// unit, so an exit would need a state the kernel does not have yet.
[[noreturn]] void shell();

}  // namespace proc
