#include "hart.h"

#include "riscv.h"

using namespace riscv::registers;

// Per-hart state, indexed by the dense cpuid assigned in start.S.
//
// _entry hands out cpuid 0 to the boot hart and 1, 2, ... to the secondaries in
// arrival order, so cpuid() is always in [0, MAX_HARTS) and never
// the sparse hardware hartid. The entry stub parks a hart that finds no stack
// slot, which is what keeps this array in bounds even when the machine has more
// harts than the pool.
//
// The mapping from hardware hartid to cpuid is not recorded. Nothing currently
// needs it: kmain receives the hartid as an argument. Code that needs to go
// from one to the other has to build a hartid -> cpuid table from the tree.
static cpu g_cpus[MAX_HARTS];

uint64_t cpuid() noexcept {
    return riscv::read(tp);
}

cpu* mycpu() noexcept {
    return &g_cpus[cpuid()];
}
