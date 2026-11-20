/* QEMU `virt` RAM starts at 0x8000_0000. Keep the kernel in the first 128 MiB. */
OUTPUT_ARCH(riscv)
ENTRY(_entry)

/*
 * Kernel image layout.
 *
 * Sections are grouped by the permissions they need, and only the first section
 * of a group starts on a 4 KiB page boundary -- the page size the kernel uses
 * for paging, so 4096 below must match riscv::PGSIZE in riscv.h. Sections that
 * share the permissions of the one before them are packed with their natural
 * alignment only, so the image does not waste a page per section:
 *
 *   .text       R+X  code
 *   .rodata     R    read-only data, including .srodata
 *   .data       R+W  initialized writable data, including .sdata
 *   .init_array R+W  pointers to the static constructors
 *   .bss        R+W  zero-initialized data, including .sbss
 *   .stack      R+W  the per-hart boot stacks (no file contents), after _end
 *
 * The __<section>_start / __<section>_end symbols bracket each section and are
 * declared in src/kernel/mm/vm.h for the page-table code. _start and _end span
 * the kernel image proper -- code, data and bss -- with _end rounded up to a
 * page so the physical allocator can keep the kernel out of the free pool page
 * by page. The .stack pool sits after _end, so it is a separate range the
 * allocator can account for independently of the image overlap check.
 */
SECTIONS {
  . = 0x80000000;
  . = ALIGN(4096);

  /* First byte of the kernel image; _entry (start.S) lives here. */
  _start = .;

  .text : {
    __text_start = .;
    KEEP(*(.text.boot))
    . = ALIGN(4096);
    _trampoline = .;
    /* The page is entered through stvec, never called from C, so nothing
       references trampoline/usertvec: --gc-sections would discard the whole
       input section and the ASSERT below would then see zero bytes, not a
       trampoline that is too big. Keep it explicitly. */
    KEEP(*(.text.tvec))
    . = ALIGN(4096);
    ASSERT(. - _trampoline == 0x1000,
           "error: trampoline must occupy exactly one page");
    *(.text .text.*)
    __text_end = .;
  }

  . = ALIGN(4096);
  .rodata : {
    __rodata_start = .;
    *(.rodata .rodata.*) *(.srodata .srodata.*)
    __rodata_end = .;
  }

  . = ALIGN(4096);
  .data : {
    __data_start = .;
    *(.data .data.*)
    /* Small data is addressed relative to gp with a signed 12-bit offset when
       the linker can relax it, so .sdata has to stay within 2 KiB of the
       pointer. Keep it at the end of .data, right after __global_pointer$. */
    . = ALIGN(8);
    __global_pointer$ = . + 0x800;
    *(.sdata .sdata.*)
    __data_end = .;
  }

  .init_array : {
    __init_array_start = .;
    KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
    KEEP(*(.init_array))
    __init_array_end = .;
  }

  .bss : {
    __bss_start = .;
    *(.sbss .sbss.*) *(.bss .bss.*) *(COMMON)
    __bss_end = .;
  }

  /* First byte past the kernel image (code + data + bss), rounded up to a whole
     page. */
  . = ALIGN(4096);
  _end = .;

  /* The boot stack pool keeps its own section so it has explicit bounds and
     stays outside the [_start, _end) range the physical allocator protects. It
     is page aligned because every per-hart stack in it is page aligned: mm/vm.cc
     maps the pool in the identity map as one run, each hart later frees its
     slice page by page, and a page must not be shared with the section before
     it or with the next stack. */
  .stack (NOLOAD) : {
    __boot_stack_start = .;
    *(.stack .stack.*)
    __boot_stack_end = .;
  }

  /* start.S computes per-hart stack addresses by adding hard-coded offsets to
     boot_stacks, so the object has to be the first thing in .stack, and the
     whole pool has to stay page aligned / page sized for the identity mapping
     and the per-slice free path. Fail the link instead of running harts on a
     shifted stack. */
  ASSERT(boot_stacks == __boot_stack_start,
         "boot_stacks must be the first object in .stack")
  ASSERT((__boot_stack_start & 4095) == 0,
         ".stack must be page aligned")
  ASSERT(((__boot_stack_end - __boot_stack_start) & 4095) == 0,
         ".stack must be a whole number of pages")

  /* mm/vm.cc maps one permission group per page run: it maps
     [__text_start, __rodata_start), [__rodata_start, __data_start) and
     [__data_start, _end), and relies on every boundary being page aligned.
     Catch a layout change at link time instead of at the boot-time mappages()
     assertion. */
  ASSERT((__text_start & 4095) == 0, ".text must start on a page boundary")
  ASSERT((__rodata_start & 4095) == 0, ".rodata must start on a page boundary")
  ASSERT((__data_start & 4095) == 0, ".data must start on a page boundary")
  ASSERT((_end & 4095) == 0, "_end must be page aligned")
}
