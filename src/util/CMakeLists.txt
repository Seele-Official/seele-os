# Freestanding utilities shared by both the kernel and user programs.
add_library(seele_util STATIC
    format.cc
)
target_include_directories(seele_util PUBLIC
    "${PROJECT_SOURCE_DIR}/src"
)
target_compile_definitions(seele_util PRIVATE
    $<$<CONFIG:Debug>:_GLIBCXX_ASSERTIONS>
)
target_link_libraries(seele_util PRIVATE
    seele_freestanding_options
)
