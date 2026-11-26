/* User-space ELF64 linker script for seele-os (RV64GC, Sv39 user window). */
OUTPUT_ARCH(riscv)
ENTRY(_start)

/*
 * User virtual space occupies Sv39 root entries 128..255
 * [0x0000_0020_0000_0000, 0x0000_0040_0000_0000). Programs link at
 * USER_BASE + 0x10000 (leaving a 64 KiB unmapped gap below) with three
 * page-aligned W^X PT_LOAD segments:
 *
 *   text    PF_R | PF_X   code (.text.start first)
 *   rodata  PF_R          read-only data
 *   data    PF_R | PF_W   initialized data, .init_array, and .bss
 */
PHDRS {
  text   PT_LOAD FLAGS(5); /* PF_R | PF_X */
  rodata PT_LOAD FLAGS(4); /* PF_R */
  data   PT_LOAD FLAGS(6); /* PF_R | PF_W */
}

SECTIONS {
  . = 0x0000002000010000;
  . = ALIGN(4096);

  .text : {
    __user_text_start = .;
    KEEP(*(.text.start))
    *(.text .text.*)
    __user_text_end = .;
  } :text

  . = ALIGN(4096);
  .rodata : {
    __user_rodata_start = .;
    *(.rodata .rodata.*) *(.srodata .srodata.*)
    __user_rodata_end = .;
  } :rodata

  . = ALIGN(4096);
  .data : {
    __user_data_start = .;
    *(.data .data.*) *(.sdata .sdata.*)
    __user_data_end = .;
  } :data

  .init_array : {
    __init_array_start = .;
    KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
    KEEP(*(.init_array))
    __init_array_end = .;
  } :data

  .bss : {
    __user_bss_start = .;
    *(.sbss .sbss.*) *(.bss .bss.*) *(COMMON)
    __user_bss_end = .;
  } :data

  /DISCARD/ : {
    *(.eh_frame .eh_frame.*)
    *(.comment)
    *(.note .note.*)
  }

  ASSERT((__user_text_start & 4095) == 0, "user .text must be page aligned")
  ASSERT((__user_rodata_start & 4095) == 0, "user .rodata must be page aligned")
  ASSERT((__user_data_start & 4095) == 0, "user .data must be page aligned")
}
