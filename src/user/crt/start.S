# User-space entry point for seele-os executables.
#
# The kernel ELF loader maps PT_LOAD segments (zeroing .bss), maps the initial
# 16-byte aligned user stack, and enters _start in U-mode via sret. Clear s0
# to terminate frame-pointer walks cleanly, then transfer to C++ __user_start.

.section .text.start,"ax",@progbits
.globl _start
.type _start,@function
_start:
    mv s0, zero
    call __user_start
1:
    j 1b
    .size _start, .-_start
