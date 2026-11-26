# -----------------------------------------------------------------------------
# Kernel build parameters and configure-time validation
# -----------------------------------------------------------------------------
#
# These used to be hard-coded in the header or source file they describe. They
# are cache variables now and reach the kernel as -D, so a board port or a size
# experiment is a configure flag instead of a source edit:
#
#   cmake --preset qemu-virt -DSEELE_MAX_HARTS=8 -DSEELE_BOOT_STACK_SIZE=32768
#
# The headers do not fall back to a default: a build that forgets one fails at
# the #error next to its use rather than silently compiling a kernel that
# differs from the one on the command line. For the same reason the checks
# below run at configure time, where the message can name the flag.

set(SEELE_LINKER_SCRIPT
    "${PROJECT_SOURCE_DIR}/src/kernel/linker.ld"
    CACHE FILEPATH "Kernel linker script")

# SEELE_MAX_HARTS is the one hart bound: it is MAX_HARTS in hart.h, it sizes
# platform::Info's harts, the per-hart cpus[] table and the boot stack pool (one
# dense cpuid slot each). A hart whose dense cpuid does not fit is parked by
# _entry instead of running. There is deliberately no second "how many boot
# stacks" knob: two numbers that must agree are one number, and deriving it
# keeps cpus[], the pool layout and start.S from drifting apart. Kernel stacks
# are per process, not per hart, so they are not bounded by this number.
set(SEELE_MAX_HARTS 64 CACHE STRING "Maximum number of harts, cpuid slots and boot stacks")
set(SEELE_BOOT_STACK_SIZE 65536 CACHE STRING "Stack size of the boot hart's boot stack, in bytes")
set(SEELE_SECONDARY_STACK_SIZE 8192 CACHE STRING "Boot stack size of every secondary hart, in bytes")
set(SEELE_KSTACK_SIZE 16384 CACHE STRING "Kernel stack each thread maps, in bytes")

# Capacity of the fixed-size tables that hold firmware descriptions. Firmware
# that exceeds them is truncated with a warning, never fatal, so these trade
# static memory for how much of a machine the kernel can describe.
set(SEELE_MAX_REGIONS 32 CACHE STRING "Platform physical memory region capacity")
set(SEELE_MAX_DEVICES 48 CACHE STRING "Platform bus device capacity")
set(SEELE_MAX_RESOURCES 128 CACHE STRING "Platform bus resource capacity")
set(SEELE_MAX_DRIVERS 32 CACHE STRING "Driver probe order capacity")
# One entry per interrupt source the kernel routes to a driver. This is trap's
# linear external-handler table (trap.cc), not the controller's source count
# (riscv,ndev): only sources some driver registers need a slot, and a full
# table is a refused registration, not a bound that has to cover every line on
# the board.
set(SEELE_MAX_IRQ_HANDLERS 16 CACHE STRING "Trap external-handler table capacity")
set(SEELE_FDT_MAX_DEPTH 32 CACHE STRING "Maximum depth the FDT walkers descend")

set(SEELE_UART_BAUD 115200 CACHE STRING "NS16550 line rate in bits per second")

option(SEELE_TESTS "Compile the in-kernel KTest runner and test suites" OFF)
if(SEELE_TESTS)
    set(SEELE_TESTS_VALUE 1)
else()
    set(SEELE_TESTS_VALUE 0)
endif()

# Validate and normalize the integers above. `LESS`/`GREATER` on two strings
# would compare lexicographically, so every parameter is first checked to be a
# non-negative integer.
function(seele_require_positive_integer name)
    if(NOT "${${name}}" MATCHES "^[0-9]+$")
        message(FATAL_ERROR
            "seele: ${name} must be a positive integer, got '${${name}}'")
    endif()
    math(EXPR value "${${name}}" OUTPUT_FORMAT DECIMAL)
    if(value LESS 1)
        message(FATAL_ERROR "seele: ${name} must be at least 1, got ${value}")
    endif()
    set(${name} "${value}" PARENT_SCOPE)
endfunction()

function(seele_require_page_multiple name)
    math(EXPR remainder "${${name}} % 4096")
    if(NOT remainder EQUAL 0)
        message(FATAL_ERROR
            "seele: ${name} must be a multiple of the 4 KiB page size, got ${${name}}")
    endif()
endfunction()

foreach(parameter
        SEELE_MAX_HARTS
        SEELE_BOOT_STACK_SIZE
        SEELE_SECONDARY_STACK_SIZE
        SEELE_KSTACK_SIZE
        SEELE_MAX_REGIONS
        SEELE_MAX_DEVICES
        SEELE_MAX_RESOURCES
        SEELE_MAX_DRIVERS
        SEELE_MAX_IRQ_HANDLERS
        SEELE_FDT_MAX_DEPTH
        SEELE_UART_BAUD)
    seele_require_positive_integer(${parameter})
endforeach()

foreach(parameter
        SEELE_BOOT_STACK_SIZE
        SEELE_SECONDARY_STACK_SIZE
        SEELE_KSTACK_SIZE)
    seele_require_page_multiple(${parameter})
endforeach()

# platform/device.h stores a device's first-resource index and its resource
# count in uint16_t fields, so a larger table would truncate rather than grow.
if(SEELE_MAX_RESOURCES GREATER 65535)
    message(FATAL_ERROR
        "seele: SEELE_MAX_RESOURCES (${SEELE_MAX_RESOURCES}) does not fit the uint16_t "
        "resource fields in platform/device.h (at most 65535)")
endif()
if(SEELE_UART_BAUD GREATER 4000000)
    # 16 * baud must fit the 16-bit divisor the LCR bank takes, so anything
    # above 4 Mbaud cannot be programmed even by the fastest input clock.
    message(FATAL_ERROR "seele: SEELE_UART_BAUD (${SEELE_UART_BAUD}) is not programmable")
endif()

# Every parameter as a -D. target_compile_definitions() reaches both C++ and
# ASM sources (start.S includes boot/stack.h), which is what lets the assembly
# and the headers agree on the same numbers.
set(SEELE_KERNEL_DEFINITIONS
    SEELE_MAX_HARTS=${SEELE_MAX_HARTS}
    SEELE_BOOT_STACK_SIZE=${SEELE_BOOT_STACK_SIZE}
    SEELE_SECONDARY_STACK_SIZE=${SEELE_SECONDARY_STACK_SIZE}
    SEELE_KSTACK_SIZE=${SEELE_KSTACK_SIZE}
    SEELE_MAX_REGIONS=${SEELE_MAX_REGIONS}
    SEELE_MAX_DEVICES=${SEELE_MAX_DEVICES}
    SEELE_MAX_RESOURCES=${SEELE_MAX_RESOURCES}
    SEELE_MAX_DRIVERS=${SEELE_MAX_DRIVERS}
    SEELE_MAX_IRQ_HANDLERS=${SEELE_MAX_IRQ_HANDLERS}
    SEELE_FDT_MAX_DEPTH=${SEELE_FDT_MAX_DEPTH}
    SEELE_UART_BAUD=${SEELE_UART_BAUD}
    SEELE_TESTS=${SEELE_TESTS_VALUE}
)

message(STATUS "seele-os linker: ${SEELE_LINKER_SCRIPT}")
message(STATUS
    "seele-os harts: max=${SEELE_MAX_HARTS} (cpuid slots, boot stacks, cpus[]) "
    "boot_stack=${SEELE_BOOT_STACK_SIZE}B secondary_stack=${SEELE_SECONDARY_STACK_SIZE}B "
    "kstack=${SEELE_KSTACK_SIZE}B")
message(STATUS
    "seele-os capacity: regions=${SEELE_MAX_REGIONS} devices=${SEELE_MAX_DEVICES} "
    "resources=${SEELE_MAX_RESOURCES} drivers=${SEELE_MAX_DRIVERS} "
    "irq_handlers=${SEELE_MAX_IRQ_HANDLERS} fdt_depth=${SEELE_FDT_MAX_DEPTH} "
    "uart_baud=${SEELE_UART_BAUD} tests=${SEELE_TESTS}")
