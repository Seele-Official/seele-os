#pragma once

#include "proc/process.h"

// Thread switching: the one operation the scheduler performs.
//
// The thread type itself lives in proc/process.h, because a thread is tied to
// its process (it points at it and uses its kernel stack window). This header
// only exposes the assembly half.
namespace shed {

// Save `from`'s context and resume `to` (defined in shed/switch.S). When the
// two threads belong to different processes, the switch loads to->proc->satp
// into satp (with a TLB flush); a same-process switch leaves satp alone.
//
// Called from the trap path, and from a thread that parks itself in shed::sleep()
// (which is how console::read() blocks). Both callers switch with interrupts
// disabled and no lock held: switch_thread() is an ordinary call, so the
// compiler has spilled whatever the caller still needs, and this routine saves
// the rest of the callee-saved state per thread.
extern "C" void switch_thread(proc::thread* from, proc::thread* to);

}  // namespace shed
