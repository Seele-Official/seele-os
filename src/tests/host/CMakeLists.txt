find_package(GTest CONFIG REQUIRED)
find_path(LIBFDT_INCLUDE_DIR NAMES libfdt.h REQUIRED)
find_library(LIBFDT_LIBRARY NAMES fdt REQUIRED)
include(GoogleTest)

add_executable(seele_host_tests
    "${PROJECT_SOURCE_DIR}/src/kernel/fdt/fdt.cc"
    "${PROJECT_SOURCE_DIR}/src/kernel/proc/elf_format.cc"
    "${PROJECT_SOURCE_DIR}/src/util/format.cc"
    panic_stub.cc
    addr_test.cc
    elf_test.cc
    fdt_test.cc
    format_and_device_test.cc
    list_test.cc
)
set_target_properties(seele_host_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
target_include_directories(seele_host_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src/kernel"
    "${PROJECT_SOURCE_DIR}/src"
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

gtest_discover_tests(seele_host_tests)
