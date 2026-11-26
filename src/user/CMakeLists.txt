# -----------------------------------------------------------------------------
# User-space CRT and freestanding user executables
# -----------------------------------------------------------------------------

set(SEELE_USER_LINKER_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/linker.ld")
set(SEELE_USER_OUTPUT_DIR "${CMAKE_BINARY_DIR}/user")

find_program(SEELE_OBJCOPY NAMES riscv64-unknown-elf-objcopy REQUIRED)

add_library(seele_user_crt STATIC
    crt/start.S
    crt/io.cc
    crt/runtime.cc
)
target_include_directories(seele_user_crt PUBLIC
    "${PROJECT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_compile_definitions(seele_user_crt PRIVATE
    $<$<CONFIG:Debug>:_GLIBCXX_ASSERTIONS>
)
target_link_libraries(seele_user_crt
    PRIVATE seele_freestanding_options
    PUBLIC seele_string seele_util
)

# Define a freestanding user-space executable linked against seele_user_crt,
# plus a matching `<target>_embed` custom target that strips debug/symbol
# sections into `<buildDir>/user/<output>.stripped.elf` for kernel #embed use.
function(seele_add_user_executable target)
    cmake_parse_arguments(ARG "" "OUTPUT_NAME" "SOURCES" ${ARGN})
    if(NOT ARG_OUTPUT_NAME)
        set(ARG_OUTPUT_NAME "${target}")
    endif()
    if(NOT ARG_SOURCES)
        message(FATAL_ERROR
            "seele_add_user_executable(${target}): SOURCES is required")
    endif()

    add_executable(${target} ${ARG_SOURCES})
    set_target_properties(${target} PROPERTIES
        OUTPUT_NAME "${ARG_OUTPUT_NAME}"
        SUFFIX ".elf"
        LINK_DEPENDS "${SEELE_USER_LINKER_SCRIPT}"
        RUNTIME_OUTPUT_DIRECTORY "${SEELE_USER_OUTPUT_DIR}"
    )
    target_compile_definitions(${target} PRIVATE
        $<$<CONFIG:Debug>:_GLIBCXX_ASSERTIONS>
    )
    target_link_options(${target} PRIVATE
        -nostdlib
        -nostartfiles
        -static
        -Wl,-T,${SEELE_USER_LINKER_SCRIPT}
        -Wl,--gc-sections
        -Wl,--build-id=none
        -Wl,--no-relax-gp
    )
    target_link_libraries(${target} PRIVATE
        seele_freestanding_options
        seele_user_crt
    )

    set(embed_path "${SEELE_USER_OUTPUT_DIR}/${ARG_OUTPUT_NAME}.stripped.elf")
    # Strip debug/symbol sections for #embed while keeping the full unstripped
    # <buildDir>/user/<output>.elf alongside it for debugging and symbolization.
    add_custom_command(
        OUTPUT "${embed_path}"
        COMMAND "${SEELE_OBJCOPY}" --strip-all "$<TARGET_FILE:${target}>" "${embed_path}"
        DEPENDS ${target}
        COMMENT "Stripping user ${ARG_OUTPUT_NAME} ELF for #embed"
        VERBATIM
    )
    add_custom_target(${target}_embed
        DEPENDS "${embed_path}"
    )
    set_target_properties(${target}_embed PROPERTIES
        SEELE_EMBED_DIR "${SEELE_USER_OUTPUT_DIR}"
        SEELE_EMBED_FILE "${embed_path}"
    )
endfunction()

seele_add_user_executable(user_shell
    OUTPUT_NAME shell
    SOURCES
        shell/main.cc
)
