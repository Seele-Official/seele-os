cmake_minimum_required(VERSION 3.25)

option(SEELE_HOST_TESTS "Build host-side unit tests with ASan and UBSan" OFF)

if(SEELE_HOST_TESTS)
    project(seele_os_host_tests
        VERSION 0.1.0
        LANGUAGES CXX
    )

    if(CMAKE_SYSTEM_PROCESSOR STREQUAL "riscv64")
        message(FATAL_ERROR
            "SEELE_HOST_TESTS builds native host tests; configure without the RISC-V toolchain file")
    endif()

    set(CMAKE_CXX_STANDARD 26)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

    find_package(GTest CONFIG REQUIRED)
    find_path(LIBFDT_INCLUDE_DIR NAMES libfdt.h REQUIRED)
    find_library(LIBFDT_LIBRARY NAMES fdt REQUIRED)
    include(GoogleTest)

    add_executable(seele_host_tests
        src/kernel/fdt/fdt.cc
        src/kernel/util/format.cc
        src/tests/host/panic_stub.cc
        src/tests/host/addr_test.cc
        src/tests/host/fdt_test.cc
        src/tests/host/format_and_device_test.cc
        src/tests/host/list_test.cc
    )
    target_include_directories(seele_host_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/kernel"
    )
    target_include_directories(seele_host_tests SYSTEM PRIVATE
        "${LIBFDT_INCLUDE_DIR}"
    )
    target_compile_definitions(seele_host_tests PRIVATE
        SEELE_FDT_MAX_DEPTH=32
    )
    target_compile_options(seele_host_tests PRIVATE
        -O1
        -g
        -fno-omit-frame-pointer
        -fsanitize=address,undefined
        -fno-sanitize-recover=all
        -Wall
        -Wextra
        -Wpedantic
    )
    target_link_options(seele_host_tests PRIVATE
        -fsanitize=address,undefined
    )
    target_link_libraries(seele_host_tests PRIVATE
        GTest::gtest_main
        "${LIBFDT_LIBRARY}"
    )

    enable_testing()
    gtest_discover_tests(seele_host_tests)
    return()
endif()

project(seele_os
    VERSION 0.1.0
    LANGUAGES CXX ASM
)

if(NOT CMAKE_SYSTEM_PROCESSOR STREQUAL "riscv64")
    message(FATAL_ERROR
        "This project must be configured with the RISC-V toolchain file:\n"
        "  -DCMAKE_TOOLCHAIN_FILE=cmake/riscv64-unknown-elf.cmake")
endif()

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")
set(CMAKE_ASM_FLAGS_RELEASE "-O2")

# RV64GC = IMAFD + C, plus Zicsr/Zifencei for M-mode CSR access and the
# standard F/D floating-point registers. The ABI is lp64d (hardware double).
set(SEELE_RISCV_ARCH   "rv64gc"  CACHE STRING "RISC-V ISA string")
set(SEELE_RISCV_ABI    "lp64d"   CACHE STRING "RISC-V ABI")
set(SEELE_RISCV_CMODEL "medany"  CACHE STRING "RISC-V code model")

set(SEELE_LINKER_SCRIPT
    "${CMAKE_CURRENT_SOURCE_DIR}/src/kernel/linker.ld"
    CACHE FILEPATH "Kernel linker script")


# -----------------------------------------------------------------------------
# Kernel build parameters
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
#
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


set(SEELE_COMPILE_OPTIONS
    -march=${SEELE_RISCV_ARCH}
    -mabi=${SEELE_RISCV_ABI}
    -mcmodel=${SEELE_RISCV_CMODEL}
    -ffreestanding
    -fno-exceptions
    -fno-rtti
    -fno-threadsafe-statics
    -fno-use-cxa-atexit
    -fno-stack-protector
    -fno-pic
    -fno-pie
    # Unwind tables are not needed for C++ (exceptions are off), but the kernel
    # does need to walk a call stack when it panics. Keeping s0 as a frame
    # pointer is what stacktrace.cc reads: see src/kernel/stacktrace.h. It costs
    # a 16-byte save area and a few instructions in every function.
    -fno-omit-frame-pointer
    -fno-asynchronous-unwind-tables
    -fno-unwind-tables
    -fno-ident
    -ffunction-sections
    -fdata-sections
    -g
    -Wall
    -Wextra
    -Wpedantic
)


# The `riscv64-unknown-elf-gcc` pixi package provides the compiler, binutils
# and the freestanding libstdc++ headers.  The kernel still provides its own
# C library, C++ ABI and heap, and does not link libstdc++/libc.
#
# start.S must stay assembly: _entry runs before a stack exists and does the
# per-hart stack selection. Do not merge it into start.cc: the compiler is free
# to emit a prologue for a C++ function, so "does not use the stack" is not a
# property the build can guarantee.
add_executable(kernel
    src/kernel/start.S
    src/kernel/start.cc
    src/kernel/kmain.cc
    src/kernel/panic.cc
    src/kernel/power.cc
    src/kernel/trap.cc
    src/kernel/tvec.S
    src/kernel/console.cc
    src/kernel/hart.cc
    src/kernel/stacktrace.cc
    src/kernel/concurrency/spinlock.cc
    src/kernel/concurrency/wait_queue.cc
    src/kernel/concurrency/semaphore.cc
    src/kernel/fdt/fdt.cc
    src/kernel/platform/early_console.cc
    src/kernel/platform/platform.cc
    src/kernel/platform/device.cc
    src/kernel/drivers/registry.cc
    src/kernel/drivers/table.cc
    src/kernel/drivers/irq/plic.cc
    src/kernel/drivers/power/sifive_test.cc
    src/kernel/drivers/uart/ns16550.cc
    src/kernel/mm/kalloc.cc
    src/kernel/mm/heap.cc
    src/kernel/mm/vm.cc
    src/kernel/proc/process.cc
    src/kernel/proc/shell.cc
    src/kernel/sched/switch.S
    src/kernel/sched/schedule.cc
    src/kernel/util/format.cc
    src/cxx/runtime.cc
)

if(SEELE_TESTS)
    target_sources(kernel PRIVATE
        src/kernel/test/ktest.cc
        src/kernel/test/mm_test.cc
        src/kernel/test/platform_test.cc
        src/kernel/test/sync_test.cc
    )
endif()

set_target_properties(kernel PROPERTIES
    OUTPUT_NAME "kernel"
    SUFFIX ".elf"
    LINK_DEPENDS "${SEELE_LINKER_SCRIPT}"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

target_compile_options(kernel PRIVATE
    ${SEELE_COMPILE_OPTIONS}
)

# Kernel headers are included as "fdt/fdt.h" and "mm/addr.h", i.e. relative to
# the kernel source root rather than to the file doing the include.
target_include_directories(kernel PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/kernel")

# libstdc++ turns its own precondition checks on when _GLIBCXX_ASSERTIONS is
# defined; Release leaves them out, Debug pays for them. A failed check calls
# abort(), which the kernel routes to panic(). The kernel's
# own SEELE_ASSERT macros do not use this switch: they are always on.
target_compile_definitions(kernel PRIVATE
    ${SEELE_KERNEL_DEFINITIONS}
    $<$<CONFIG:Debug>:_GLIBCXX_ASSERTIONS>
)

target_link_options(kernel PRIVATE
    -nostdlib
    -nostartfiles
    -static
    -Wl,-T,${SEELE_LINKER_SCRIPT}
    -Wl,--gc-sections
    -Wl,--build-id=none
    # Kernel does not initialize or rely on __global_pointer$; disable GP
    # relaxation while keeping call/PC-relative relaxation enabled.
    -Wl,--no-relax-gp
)

# The kernel's own C library. It is a separate static library so it can grow
# its own surface (add files here). -fno-builtin prevents the compiler from
# turning libc implementations into recursive calls to themselves.
add_library(seele_libc STATIC
    src/libc/string.cc
    src/libc/stdlib.cc
)
target_include_directories(seele_libc PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src/libc/include")
target_compile_options(seele_libc PRIVATE
    ${SEELE_COMPILE_OPTIONS}
    -fno-builtin
)

target_link_libraries(kernel PRIVATE seele_libc)

message(STATUS "seele-os target: ${SEELE_RISCV_ARCH}/${SEELE_RISCV_ABI}, cmodel=${SEELE_RISCV_CMODEL}")
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
