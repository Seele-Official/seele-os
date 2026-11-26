include(KernelConfig)

# The `riscv64-unknown-elf-gcc` pixi package provides the compiler, binutils
# and the freestanding libstdc++ headers. The kernel still provides its own
# C library, C++ ABI and heap, and does not link libstdc++/libc.
#
# boot/start.S must stay assembly: _entry runs before a stack exists and does
# the per-hart stack selection. Do not merge it into boot/start.cc: the
# compiler is free to emit a prologue for a C++ function, so "does not use the
# stack" is not a property the build can guarantee.
add_executable(kernel
    boot/start.S
    boot/start.cc
    kmain.cc
    panic.cc
    power.cc
    trap.cc
    tvec.S
    char_device.cc
    console.cc
    hart.cc
    stacktrace.cc
    concurrency/spinlock.cc
    concurrency/wait_queue.cc
    concurrency/mutex.cc
    concurrency/semaphore.cc
    fdt/fdt.cc
    platform/early_console.cc
    platform/platform.cc
    platform/device.cc
    drivers/registry.cc
    drivers/table.cc
    drivers/irq/plic.cc
    drivers/power/sifive_test.cc
    drivers/uart/ns16550.cc
    mm/kalloc.cc
    mm/heap.cc
    mm/vm.cc
    proc/elf_format.cc
    proc/elf_loader.cc
    proc/process.cc
    proc/shell.cc
    proc/syscall.cc
    sched/switch.S
    sched/schedule.cc
    cxx/new.cc
    cxx/runtime.cc
)

if(SEELE_TESTS)
    target_sources(kernel PRIVATE
        test/ktest.cc
        test/mm_test.cc
        test/platform_test.cc
        test/sync_test.cc
        test/user_test.cc
    )
endif()

set_target_properties(kernel PROPERTIES
    OUTPUT_NAME "kernel"
    SUFFIX ".elf"
    LINK_DEPENDS "${SEELE_LINKER_SCRIPT}"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# Kernel headers are included as "fdt/fdt.h" and "mm/addr.h", i.e. relative to
# the kernel source root rather than to the file doing the include. Shared
# kernel/user UAPI headers are included as "uapi/syscall.h".
target_include_directories(kernel PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/src"
)

# libstdc++ turns its own precondition checks on when _GLIBCXX_ASSERTIONS is
# defined; Release leaves them out, Debug pays for them. A failed check calls
# abort(), which the kernel routes to panic(). The kernel's own SEELE_ASSERT
# macros do not use this switch: they are always on.
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

target_link_libraries(kernel PRIVATE
    seele_freestanding_options
    seele_libc
    seele_util
)

get_target_property(SEELE_SHELL_EMBED_DIR user_shell_embed SEELE_EMBED_DIR)
get_target_property(SEELE_SHELL_EMBED_PATH user_shell_embed SEELE_EMBED_FILE)
add_dependencies(kernel user_shell_embed)
set_source_files_properties(proc/shell.cc PROPERTIES
    COMPILE_OPTIONS "--embed-dir=${SEELE_SHELL_EMBED_DIR}"
    OBJECT_DEPENDS "${SEELE_SHELL_EMBED_PATH}"
)
