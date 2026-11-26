# Freestanding C string functions shared by both the kernel and user programs.
# -fno-builtin prevents the compiler from turning implementations into
# recursive calls to themselves.
add_library(seele_string STATIC
    string.cc
)
target_include_directories(seele_string PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_compile_options(seele_string PRIVATE
    -fno-builtin
)
target_link_libraries(seele_string PRIVATE
    seele_freestanding_options
)

# The kernel's own C library: adds kernel-side abort() on top of seele_string.
add_library(seele_libc STATIC
    stdlib.cc
)
target_compile_options(seele_libc PRIVATE
    -fno-builtin
)
target_link_libraries(seele_libc
    PRIVATE seele_freestanding_options
    PUBLIC seele_string
)
