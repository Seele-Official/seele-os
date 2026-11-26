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

    enable_testing()
    add_subdirectory(src/tests/host)
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

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

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

add_library(seele_freestanding_options INTERFACE)
target_compile_options(seele_freestanding_options INTERFACE
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

message(STATUS "seele-os target: ${SEELE_RISCV_ARCH}/${SEELE_RISCV_ABI}, cmodel=${SEELE_RISCV_CMODEL}")

add_subdirectory(src/libc)
add_subdirectory(src/util)
add_subdirectory(src/user)
add_subdirectory(src/kernel)
