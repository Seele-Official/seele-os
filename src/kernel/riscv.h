#pragma once

#include <cstdint>

#include "util/static_str.h"

namespace riscv {

constexpr uint64_t PGSIZE = 4096;               // bytes per page
constexpr uint64_t PGOFFSET_MASK = PGSIZE - 1;  // low bits that index inside a page
constexpr uint64_t PGSHIFT = 12;                // bits of offset within a page
constexpr uint64_t PGFSHIFT = 10;
using PageTableEntry = uint64_t;

using PageTable = PageTableEntry[PGSIZE / sizeof(PageTableEntry)];

// PTE flag bits. A leaf entry (one with any of R/W/X set) needs at least one of
// R/W/X, W implies R, and hardware or software sets A/D. Sv39 can update A/D in
// hardware (menvcfg.ADUE), so a mapper may pass them explicitly instead. Bit 5
// is reserved in Sv39 and must stay zero.
//
//   V[0]  valid: the entry is present
//   R[1]  readable
//   W[2]  writable (implies R)
//   X[3]  executable
//   U[4]  accessible from U-mode
//   A[6]  accessed by a translation (read, write or execute)
//   D[7]  dirtied by a store
//
// Bit 5 is reserved in Sv39 and must stay zero.
constexpr uint64_t PTE_V = 0b1ull << 0;
constexpr uint64_t PTE_R = 0b1ull << 1;
constexpr uint64_t PTE_W = 0b1ull << 2;
constexpr uint64_t PTE_X = 0b1ull << 3;
constexpr uint64_t PTE_U = 0b1ull << 4;
constexpr uint64_t PTE_A = 0b1ull << 6;
constexpr uint64_t PTE_D = 0b1ull << 7;

// PTE_PPN_MASK: the physical page number field, bits 53:10, recovered by
// shifting a PTE down by PGFSHIFT. Sv39 uses PPN[43:0], so bits 54-63 are
// reserved: a PTE that sets one of them must not turn those bits into part of a
// physical address.
constexpr uint64_t PTE_PPN_MASK = (0b1ull << 44) - 1;

// PTE_LEAF_MASK: every permission/status bit above, i.e. what a leaf may carry
// in addition to V. Bits outside the mask are reserved and must be zero in a
// PTE.
constexpr uint64_t PTE_LEAF_MASK = PTE_R | PTE_W | PTE_X | PTE_U | PTE_A | PTE_D;

// A CSR or GPR named at the type level. Keeping them as distinct tag types
// prevents passing a GPR to a CSR instruction or a CSR to read()/write() at
// compile time instead of failing in the assembler.
template <auto Reg>
struct csr_register {
    static constexpr auto value = Reg;
};

template <auto Reg>
struct gpr_register {
    static constexpr auto value = Reg;
};

namespace registers {

#define CSR(name) constexpr csr_register<StaticString{#name}> name
#define GPR(name) constexpr gpr_register<StaticString{#name}> name

// M-mode CSRs, programmed by start() before it mrets into S-mode.
CSR(mstatus);
CSR(medeleg);
CSR(mideleg);
CSR(mepc);
CSR(mcounteren);
CSR(menvcfg);

CSR(pmpcfg0);
CSR(pmpaddr0);

// S-mode CSRs.
CSR(sstatus);
CSR(satp);
CSR(sie);

// S-mode trap CSRs.
//
// stvec: trap vector and mode; fields in csr::stvec.
CSR(stvec);
// sepc: the pc saved on trap entry. Bit 0 always reads as zero.
CSR(sepc);
// scause: the trap cause; fields in csr::scause.
CSR(scause);
// stval: cause-specific trap value: the faulting address for the memory
// exceptions, the offending instruction bits for an illegal instruction, and
// so on.
CSR(stval);

CSR(sscratch);

CSR(stimecmp);

CSR(time);

// tp is a GPR (thread pointer), not a CSR: only read()/write() accept it.
GPR(tp);

#undef GPR
#undef CSR

}  // namespace registers

// Field encodings of the CSRs the kernel programs, grouped by CSR. They stay
// plain uint64_t masks/values so they can be combined with the csr* helpers
// below and ordinary integer arithmetic.
namespace csr {

namespace mstatus {
// FS[14:13]: 00 = Off, 01 = Initial, 10 = Clean, 11 = Dirty. Any F/D
// instruction traps while FS is Off, even if the hart implements F/D.
constexpr uint64_t FS_MASK = 0b11ull << 13;
constexpr uint64_t FS_INITIAL = 0b01ull << 13;

// MPP[12:11]: the privilege mode mret returns to. 00 = U, 01 = S, 11 = M.
constexpr uint64_t MPP_MASK = 0b11ull << 11;
constexpr uint64_t MPP_S = 0b01ull << 11;
}  // namespace mstatus

namespace sstatus {
// SIE[1]: global interrupt enable for S-mode. While it is clear no S-mode
// interrupt is taken; the sources stay pending rather than being dropped.
constexpr uint64_t SIE = 0b1ull << 1;

// SPIE[5]: indicates whether supervisor interrupts were enabled prior to trapping into supervisor
// mode When a trap is taken into supervisor mode, SPIE is set to SIE, and SIE is set to 0. When an
// SRET instruction is executed, SIE is set to SPIE, then SPIE is set to 1.
constexpr uint64_t SPIE = 0b1ull << 5;

// SPP[8]: the privilege mode sret returns to: 0 = U, 1 = S. Trap entry saves
// the interrupted mode here, and sret switches to it and then clears SPP to U.
// A handler that means to resume in S-mode must therefore preserve and restore
// the bit itself.
constexpr uint64_t SPP = 0b1ull << 8;
}  // namespace sstatus

namespace stvec {
// BASE[63:2]: address of the trap handler. Bits 1:0 carry the mode, so the
// handler must be 4-byte aligned.
constexpr uint64_t BASE_MASK = ~0b11ull;

// MODE[1:0]: 0 = Direct, 1 = Vectored, 2/3 reserved. Direct sends every trap to
// BASE; Vectored sends exceptions to BASE and interrupts to BASE + 4 * cause.
constexpr uint64_t MODE_MASK = 0b11ull;
constexpr uint64_t MODE_DIRECT = 0b00ull;
constexpr uint64_t MODE_VECTORED = 0b01ull;
}  // namespace stvec

namespace pmpcfg {
// The low byte of pmpcfg0 describes PMP entry 0.
//
// R[0]: readable.
constexpr uint64_t R = 0b1ull << 0;

// W[1]: writable.
constexpr uint64_t W = 0b1ull << 1;

// X[2]: executable.
constexpr uint64_t X = 0b1ull << 2;

// A[4:3]: address-matching mode: 0 = Off, 1 = TOR (top of range), 2 = NA4,
// 3 = NAPOT.
constexpr uint64_t A_TOR = 0b01ull << 3;

// RWX_TOR: entry 0 with R|W|X and A = TOR (0x0f).
constexpr uint64_t RWX_TOR = R | W | X | A_TOR;
}  // namespace pmpcfg

namespace mcounteren {
// mcounteren gates counter reads from S-mode (and, through scounteren, from
// U-mode). When one of these bits is clear, reading the matching counter in
// S-mode raises an illegal-instruction exception instead of returning a value.
//
// CY[0]: the cycle counter.
constexpr uint64_t CY = 0b1ull << 0;

// TM[1]: the time counter.
constexpr uint64_t TM = 0b1ull << 1;

// IR[2]: the instret counter.
constexpr uint64_t IR = 0b1ull << 2;

// HPMn[n], n in [3, 31]: hpmcountern.
constexpr uint64_t HPM_MASK = 0xfffffff8ull;

// ALL: every counter bit above. Bits for counters the hart does not implement
// are WARL and simply read back as zero.
constexpr uint64_t ALL = CY | TM | IR | HPM_MASK;
}  // namespace mcounteren

namespace menvcfg {
// ADUE[61]: controls whether hardware updating of PTE A/D bits is enabled for
// S-mode and G-stage address translations (Svadu).
constexpr uint64_t ADUE = 0b1ull << 61;

// STCE[63]: enables the Sstc extension, which lets S-mode program stimecmp
// and take timer interrupts directly instead of trapping to M-mode.
constexpr uint64_t STCE = 0b1ull << 63;
}  // namespace menvcfg

namespace sie {
// Per-source interrupt enables for interrupts delegated to S-mode; sstatus.SIE
// must also be set for any of them to be delivered.
//
// SSIE[1]: S-mode software interrupt enable.
constexpr uint64_t SSIE = 0b1ull << 1;

// STIE[5]: S-mode timer interrupt enable.
constexpr uint64_t STIE = 0b1ull << 5;

// SEIE[9]: S-mode external interrupt enable.
constexpr uint64_t SEIE = 0b1ull << 9;

}  // namespace sie

namespace scause {
// INTERRUPT[63]: 0 = the code is an exception, 1 = it is an interrupt.
constexpr uint64_t INTERRUPT = 0b1ull << 63;

// CODE[62:0]: the exception code or interrupt cause. The S-mode interrupt
// causes are the bit *indices* of the matching sie enables: SSI = 1, STI = 5,
// SEI = 9. An enable is 1 << index, so a mask must not be ORed into a cause.
constexpr uint64_t CODE_MASK = (0b1ull << 63) - 1;

// Supervisor interrupt causes.
constexpr uint64_t SSI = 1;
constexpr uint64_t STI = 5;
constexpr uint64_t SEI = 9;

// Supervisor timer interrupt
constexpr uint64_t TIME_INT = INTERRUPT | STI;
// Supervisor external interrupt
constexpr uint64_t EXT_INT = INTERRUPT | SEI;

// Environment call from U-mode
constexpr uint64_t U_ECALL = 8;
// Load page fault
constexpr uint64_t LOAD_PAGE_FAULT = 13;
// Store/AMO page fault
constexpr uint64_t STORE_PAGE_FAULT = 15;

}  // namespace scause

namespace satp {
// MODE[63:60]: 0 = Bare (no translation, addresses are physical), 8 = Sv39.
constexpr uint64_t MODE_MASK = 0xfull << 60;
constexpr uint64_t MODE_SV39 = 8ull << 60;

// PPN[43:0]: physical page number of the root page table. The page offset
// inside it is PGSHIFT bits, so the root address is PPN << PGSHIFT.
constexpr uint64_t PPN_MASK = (0b1ull << 44) - 1;
}  // namespace satp

}  // namespace csr

#ifdef __riscv

// CSR access instructions (Zicsr).
//
// Each instruction reads the old CSR value, then writes a new one, sets bits,
// or clears bits. The register forms take their operand from a GPR:
//
//   csrrw  write `value`
//   csrrs  set the bits in `mask`
//   csrrc  clear the bits in `mask`
//
// and the immediate forms csrrwi/csrrsi/csrrci do the same with a 5-bit
// unsigned zimm instead of a GPR, so their immediate must be a compile-time
// constant. The pseudo-instructions csrr, csrw, csrs and csrc are the "the old
// value is uninteresting" spellings of csrrs/csrrw/csrrs/csrrc with rd = x0.
// The non-void helpers return the old value, i.e. the rd register of the
// instruction.
//
// Setting or clearing with a zero rs1/zimm does not write the CSR at all, so
// csrs(reg, 0) cannot trigger a write side effect; csrrw always writes.
//
// Note the operand order: csrr reads a CSR into a GPR, so the assembled text is
// "csrr <rd>, <csr>", e.g. "csrr %0, sstatus"; the write helpers put the CSR
// first, as the pseudo-instructions spell it ("csrs sstatus, %0"). Operands are
// constrained with "rK": the K alternative lets GCC substitute a 0..31 constant
// directly, and GNU as then selects the matching immediate encoding, so
// csrs(sstatus, 2) assembles to csrrsi rather than a separate li.
// format_staticstr() checks the template at compile time, and the parenthesised
// expression selects GCC's constexpr-string protocol for the asm operand (an
// object with `size` and `data` members).
template <auto R>
constexpr uint64_t csrr(csr_register<R>) noexcept {
    uint64_t value;
    asm volatile((format_staticstr("csrr %0, {}", R)) : "=r"(value));
    return value;
}

template <auto R>
constexpr void csrw(csr_register<R>, uint64_t value) noexcept {
    asm volatile((format_staticstr("csrw {}, %0", R)) : : "rK"(value) : "memory");
}

template <auto R>
constexpr void csrs(csr_register<R>, uint64_t mask) noexcept {
    asm volatile((format_staticstr("csrs {}, %0", R)) : : "rK"(mask) : "memory");
}

template <auto R>
constexpr void csrc(csr_register<R>, uint64_t mask) noexcept {
    asm volatile((format_staticstr("csrc {}, %0", R)) : : "rK"(mask) : "memory");
}

// The read-modify-write forms also return the value the CSR held before the
// instruction ran. Prefer them over a csrr+csrw round trip for fields the
// hardware also updates (mstatus.FS turns Dirty on the first FP instruction,
// for example), because the read-modify-write happens inside a single CSR
// instruction.
template <auto R>
constexpr uint64_t csrrw(csr_register<R>, uint64_t value) noexcept {
    uint64_t previous;
    asm volatile((format_staticstr("csrrw %0, {}, %1", R))
                 : "=r"(previous)
                 : "rK"(value)
                 : "memory");
    return previous;
}

template <auto R>
constexpr uint64_t csrrs(csr_register<R>, uint64_t mask) noexcept {
    uint64_t previous;
    asm volatile((format_staticstr("csrrs %0, {}, %1", R))
                 : "=r"(previous)
                 : "rK"(mask)
                 : "memory");
    return previous;
}

template <auto R>
constexpr uint64_t csrrc(csr_register<R>, uint64_t mask) noexcept {
    uint64_t previous;
    asm volatile((format_staticstr("csrrc %0, {}, %1", R))
                 : "=r"(previous)
                 : "rK"(mask)
                 : "memory");
    return previous;
}

// Plain GPR access (not CSR instructions), constrained to gpr_register so a CSR
// cannot be passed here by mistake.
template <auto R>
constexpr uint64_t read(gpr_register<R>) noexcept {
    uint64_t value;
    asm volatile((format_staticstr("mv %0, {}", R)) : "=r"(value));
    return value;
}

template <auto R>
constexpr void write(gpr_register<R>, uint64_t value) noexcept {
    asm volatile((format_staticstr("mv {}, %0", R)) : : "r"(value) : "memory");
}

// Global S-mode interrupt enable (sstatus.SIE): the one bit that decides
// whether a pending source delegated to S-mode becomes a trap. intr_on() and
// intr_off() are the plain toggles; is_intr_on() reports the current state and
// changes nothing.
inline void intr_on() noexcept {
    csrs(registers::sstatus, csr::sstatus::SIE);
}

inline void intr_off() noexcept {
    csrc(registers::sstatus, csr::sstatus::SIE);
}

// Disable S-mode interrupts and report whether they were enabled before, in a
// single CSR instruction. The read-and-clear must not be split: a trap between
// a separate is_intr_on() and intr_off() can run the scheduler and change the
// state the caller meant to preserve. Pair it with intr_restore().
[[nodiscard]] inline bool intr_save() noexcept {
    return (csrrc(registers::sstatus, csr::sstatus::SIE) & csr::sstatus::SIE) != 0;
}

// Put sstatus.SIE back to the value intr_save() reported.
inline void intr_restore(bool enabled) noexcept {
    if(enabled) {
        intr_on();
    } else {
        intr_off();
    }
}

[[nodiscard]] inline bool is_intr_on() noexcept {
    return (csrr(registers::sstatus) & csr::sstatus::SIE) != 0;
}

// Wait for an interrupt. wfi may return spuriously (and with every interrupt
// disabled it can return immediately), so callers must loop around it.
inline void wfi() noexcept {
    asm volatile("wfi" : : : "memory");
}

// Make earlier stores to instruction memory visible to instruction fetch, e.g.
// after copying code.
inline void fence_i() noexcept {
    asm volatile("fence.i" : : : "memory");
}

// Flush address-translation caches after changing satp or a page table entry.
inline void sfence_vma() noexcept {
    asm volatile("sfence.vma" : : : "memory");
}

#endif  // __riscv

// mret and sret are deliberately not wrapped as functions. They do not return
// in the C sense: the instruction after them runs in the target privilege mode
// with whatever GPRs the handoff left behind. An out-of-line wrapper would
// clobber the argument registers it should hand over, and an inline one would
// hide the fact that the caller must not let the compiler emit a normal
// epilogue. The mret sequence belongs next to the code that sets mepc and
// mstatus.MPP, with an explicit ABI handoff; see enter_s_mode() in start.cc.

}  // namespace riscv
