#pragma once

// Kernel freestanding C++ runtime initialization.
namespace cxx {

// Run every global static constructor registered in .init_array.
// Must be called once by the boot hart after .bss has been zeroed and before
// any code relies on dynamically initialized static objects.
void init() noexcept;

}  // namespace cxx
