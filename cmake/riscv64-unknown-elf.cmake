# Cross-compilation toolchain for a bare-metal RISC-V 64-bit kernel.
#
# The `riscv64-unknown-elf-gcc` package in the pixi environment provides
# riscv64-unknown-elf-gcc/g++ and the matching binutils.
#
# The kernel does not link a C/C++ runtime. Keep CMake's compiler checks as
# static-library checks so they do not try to link a hosted executable.

set(CMAKE_SYSTEM_NAME      Generic)
set(CMAKE_SYSTEM_PROCESSOR riscv64)

set(CMAKE_C_COMPILER   riscv64-unknown-elf-gcc)
set(CMAKE_CXX_COMPILER riscv64-unknown-elf-g++)
set(CMAKE_ASM_COMPILER riscv64-unknown-elf-gcc)

# Compiler checks must not try to link a hosted executable.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
