# AGENTS.md

`seele-os` is a freestanding C++26 RISC-V 64-bit experimental kernel targeting QEMU `virt` (`-bios none`, boots M-mode to S-mode without OpenSBI, no user space yet).

## Operating Rules

- **Git & scope discipline.** Confirm the working directory and inspect `git status` and relevant diffs before editing. Preserve pre-existing user changes and staged state; never auto-commit. Stay strictly in scope—do not opportunistically refactor or restyle unrelated files.
- **Bound searches.** Search only `src/`, `docs/`, `tools/`, `cmake/`, and root config files. Never scan `.pixi/`, `node_modules/`, `build*/`, or `toolchain/` (managed dependencies, build outputs, or a separate repo).
- **Test suite discipline.** Use the existing in-kernel KTest suite (`src/kernel/test/`, `pixi run test-kernel`) and host-side GoogleTest + ASan/UBSan suite (`src/tests/host/`, `pixi run test-host`, aggregated under `pixi run test`). Do not add ad-hoc test harnesses or temporary automated boot scripts.
- **Language conventions.** Code comments follow surrounding source (English). Project documentation (`README.md`, `docs/`) is written in Chinese while preserving code identifiers, paths, code blocks, and units verbatim. Keep comments and subsystem docs in sync when behavior or contracts change.

## Context Routing (Read Before Editing)

Jump directly to the authoritative source and doc for your task instead of scanning the repo blindly. Use [README.md](README.md) for a high-level capability/boundary summary and [docs/conventions.md](docs/conventions.md) for naming, C++ Core Guidelines & freestanding rules, comments, and doc rules.

| Task Area                         | Primary Source Paths                                                                                          | Read First                                   |
| --------------------------------- | ------------------------------------------------------------------------------------------------------------- | -------------------------------------------- |
| Boot & cross-module design        | `src/kernel/start.S`, `start.cc`, `kmain.cc`, `hart.*`, `boot/`, `linker.ld`                                  | [docs/architecture.md](docs/architecture.md) |
| Memory, Sv39 VM & heap            | `src/kernel/mm/` (`addr.h`, `kalloc.*`, `vm.*`, `heap.*`)                                                     | [docs/memory.md](docs/memory.md)             |
| FDT, devices, drivers, I/O        | `src/kernel/fdt/`, `platform/`, `drivers/`, `console.*`, `char_device.h`, `device_object.h`                   | [docs/platform.md](docs/platform.md)         |
| Threads, traps & locks            | `src/kernel/proc/`, `src/kernel/sched/`, `trap.*`, `tvec.S`, `concurrency/`                                   | [docs/scheduling.md](docs/scheduling.md)     |
| Logging, panic, backtrace & tests | `src/kernel/console.*`, `panic.*`, `stacktrace.*`, `src/util/format.*`, `src/kernel/test/`, `src/tests/host/` | [docs/debug.md](docs/debug.md)               |
| Runtime, ABI & build tools        | `src/libc/`, `src/kernel/cxx/runtime.cc`, `src/user/crt/`, `CMakeLists.txt`, `cmake/`, `tools/`, `pixi.toml`  | [docs/build.md](docs/build.md)               |

## Hard Invariants & Gotchas

Defer detailed contracts to the docs above, but never violate these cross-cutting boundaries:

- **Freestanding ABI.** Compiled with `-fno-exceptions -fno-rtti -nostdlib -nostartfiles -static`. No host libc, libstdc++, libgcc, libatomic, compiler-rt, or `std::terminate` implementation. Avoid non-trivial static destructors (`atexit`/`__cxa_atexit`) and new runtime/compiler-helper dependencies. Build knobs (`SEELE_*`) are injected by `CMakeLists.txt` via `-D` with no header fallback defaults.
- **Address, memory & boot ordering.** Preserve `Paddr`/`Vaddr`/`phys_range` types and explicit ownership/lifetimes. Prefer static storage or driver arenas for permanent device objects over heap allocation. Shared kernel root PTEs freeze after `vm::k::init()`; the high-half heap is only valid after `vm::k::enable()`. Switching from boot context into `idle()` is one-way (boot stacks are freed there). DTB and the `platform`-owned `fdt::Tree` live for kernel lifetime; keep `fdt/` and `platform/` strictly below `drivers/` (no reverse includes).
- **Hart model & hardware limits.** Kernel indexing (`cpus[]`, locks, boot stacks) uses dense `cpuid` stored in `tp` (boot hart = hardware hart 0 = `cpuid 0`), never sparse `hartid`. There is currently no cross-hart TLB shootdown and no floating-point register context save/restore.
- **Concurrency & output locking.** Never sleep, yield, or schedule while holding a spinlock. Verify concurrency contracts before changing shared state, interrupts, page tables, or boot publication (`g_started`). Normal output lock order is `console` -> UART and UART IRQs take only the device lock; panic switches to lockless polling.

## Environment, Commands & Formatting (Pixi-First)

All commands run from the repository root via Pixi ([pixi.toml](pixi.toml) is canonical). **Never bypass Pixi** with host compilers, CMake, Ninja, `clang-format`, `pnpm`, or QEMU; never hardcode `.pixi/envs/.../bin` paths or install duplicate dependencies.

| Goal                              | Command                             | Notes                                                                    |
| --------------------------------- | ----------------------------------- | ------------------------------------------------------------------------ |
| Install workspace dependencies    | `pixi install`                      | Consumes `https://prefix.dev/seele-official/seele-os-toolchain`          |
| Install Node/TS script deps       | `pixi run setup`                    | Runs `pnpm install`                                                      |
| Configure CMake                   | `pixi run configure`                | `[buildDir] [preset]` (default `build qemu-virt`); skips if cache exists |
| Build target                      | `pixi run build`                    | `[buildDir] [preset]` (default `build qemu-virt`); runs `configure`      |
| Run all tests (host + kernel)     | `pixi run test`                     | Runs `test-host` then `test-kernel`                                      |
| Run host unit tests only          | `pixi run test-host`                | `[filter] [buildDir]` (default `* build-host`); GoogleTest + ASan/UBSan  |
| Run in-kernel KTest in QEMU only  | `pixi run test-kernel`              | `[harts] [filter] [buildDir]` (default `4 all build-test`); auto-builds  |
| Run formatting & pre-commit       | `pixi run format`                   | Runs `pre-commit run --all-files` (tracked files)                        |
| Boot QEMU (1 hart / 4 harts)      | `pixi run qemu` / `pixi run qemu 4` | `[harts] [buildDir]` (default `1 build`)                                 |
| Boot QEMU from another build dir  | `pixi run qemu 1 build-alt`         | Auto-runs `setup`, `build`, `disk`                                       |
| Regenerate 1 MiB virtio test disk | `pixi run disk`                     | `[buildDir]` (default `build`); overwrites `disk.img`                    |
| Remove a build directory          | `pixi run clean <buildDir>`         | Runs `rm -rf`; never pass repo root or user dirs                         |

- **Direct tool fallback.** Use `pixi run <tool> ...` only when no task covers the operation—for example, configuring a Debug build or changing existing CMake cache options:

    ```bash
    pixi run cmake --preset qemu-virt -B build-debug -DCMAKE_BUILD_TYPE=Debug
    pixi run build build-debug
    ```

- **Task side effects & dependencies.** Do not manually run `setup`/`build`/`disk` before `qemu` or `test-kernel`, or `build` before `test-host`. `disk`, `qemu`, and `test-kernel` all overwrite `<buildDir>/disk.img`.
- **Toolchain boundary.** `toolchain/` is a separate repo (`seele-toolchain`). Do not edit `toolchain/`, upgrade dependencies, or rewrite lockfiles for normal kernel work. Diagnose environment issues via [docs/build.md](docs/build.md) and report missing prerequisites honestly.
- **Formatting & pre-commit.**
    - Use `pixi run format` as the single entry point; never enumerate C/C++ files to run `clang-format` manually or invent formatting scripts.
    - Hook coverage ([.pre-commit-config.yaml](.pre-commit-config.yaml)): general file hygiene, C/C++ (`clang-format` from the Pixi env), and Prettier (`.ts`, `.md`, `.json`, `.yaml`/`.yml`).
    - Do not stage new files just to make `--all-files` see them. Check untracked files with `pixi run pre-commit run --files <files>`.
    - Auto-fixing hooks exit non-zero after edits: inspect the diff and re-run until clean. Never bypass failing hooks or switch to host binaries. If a full run touches unrelated files, discard those unrelated formatting edits while preserving pre-existing user changes.

## Verification & Handoff

Never treat a clean build as proof of correctness. Match verification to what changed (all tools via `pixi run`):

| Change Type                                                   | Required Verification                                                                                                                                         |
| ------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Kernel code                                                   | `pixi run build` + `pixi run test` (or targeted `test-host`/`test-kernel`) + manual QEMU shell check when shell/boot path is touched                          |
| Host-testable logic / host tests                              | `pixi run test-host` (ASan/UBSan/LSan)                                                                                                                        |
| ABI, `src/libc/`, `src/kernel/cxx/`, `src/user/crt/`, runtime | Release & Debug builds + `pixi run riscv64-unknown-elf-nm -u <dir>/kernel.elf` & `<dir>/user/shell.elf` (expect no output) per [docs/build.md](docs/build.md) |
| Struct layout, page table, boot stack                         | Verify `static_assert`s + Debug build + `pixi run test-kernel` / QEMU run                                                                                     |
| Multi-hart / shared state / locks                             | `pixi run test-kernel` (4 harts) and/or multi-hart boot (`pixi run qemu 4`)                                                                                   |
| Docs-only                                                     | Skip kernel rebuild; verify referenced commands & relative links + Prettier + `git diff --check`                                                              |

- **QEMU shutdown hygiene.** `-serial stdio -monitor none` disables `Ctrl-A X`, and `Ctrl-C` in the shell only cancels the current input line. Track and terminate only the specific QEMU process you launched; never bulk-kill unrelated QEMU instances.
- **Final handoff.** Review the full diff and run `git diff --check`. Summarize what changed, which checks actually ran (including any failures), and what remains unverified—never claim checks you did not run.
