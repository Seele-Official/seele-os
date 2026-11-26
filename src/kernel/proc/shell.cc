#include "proc/shell.h"

#include <cstddef>
#include <optional>
#include <span>

#include "console.h"
#include "panic.h"
#include "mm/addr.h"
#include "mm/vm.h"
#include "proc/elf_loader.h"
#include "proc/process.h"

namespace proc {
namespace {

alignas(8) constexpr unsigned char g_shell_elf_bytes[] = {
#embed "shell.stripped.elf"
};

}  // namespace

std::span<const std::byte> shell_image() noexcept {
    return std::as_bytes(std::span{g_shell_elf_bytes});
}

thread* spawn_shell() noexcept {
    const std::span<const std::byte> image = shell_image();
    const Paddr user_pt = vm::k::create_process_pagetable();

    const std::optional<elf::loaded_user_image> loaded = elf::load_user_elf(user_pt, image);
    if(!loaded.has_value()) {
        SEELE_PANIC("proc: failed to load embedded user shell ELF ({} bytes)", image.size());
    }

    ProcessHandle shell_proc = make_proc(user_pt);
    thread* const shell_thread = make_user_thread(shell_proc.get(), loaded->entry, loaded->user_sp);

    LOG_INFO("proc: spawned user shell (elf {} bytes, entry {:#018x}, usp {:#018x})",
             image.size(),
             to_uint64(loaded->entry),
             to_uint64(loaded->user_sp));

    return shell_thread;
}

}  // namespace proc
