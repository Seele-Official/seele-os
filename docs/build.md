# 构建与运行

本文说明工具链准备、构建配置、QEMU 运行、构建参数与 freestanding ABI 边界。最短上手流程见 [README.md](../README.md)。

## 1. 工具链准备

依赖通过 [pixi.toml](../pixi.toml) 管理，仅支持 `linux-64`（宿主机 glibc ≥ 2.28），要求 Pixi `>=0.84`。RISC-V 交叉工具链托管在 `https://prefix.dev/seele-official/seele-os-toolchain`（配方位于独立的 `seele-toolchain` 仓库）：

```bash
pixi install
```

| 包                              | 锁定版本 | 用途                    |
| ------------------------------- | -------- | ----------------------- |
| `riscv64-unknown-elf-gcc`       | 16.2.0   | 交叉编译器与 GNU 工具链 |
| `riscv64-unknown-elf-libstdcxx` | 16.2.0   | freestanding C++ 头文件 |

通过 `pixi run` 即可直接调用工具链，无需修改宿主 `PATH`。内核仅使用其中的 freestanding 头文件，不链接宿主 libc、libstdc++、libsupc++、libgcc 或 compiler-rt。

### 更新工具链包

当远程 channel 发布新版本时，在根目录仅刷新工具链包：

```bash
pixi update riscv64-unknown-elf-gcc riscv64-unknown-elf-libstdcxx
pixi install
```

更新后核对 `pixi.lock` 中的包地址与版本号。

## 2. 构建与运行

### 常用任务

以下命令均在仓库根目录执行：

```bash
pixi run build                     # 配置并构建 build/kernel.elf
pixi run qemu                      # 构建、生成磁盘并启动：1 hart，build/
pixi run qemu 4                    # 4 hart 启动
pixi run qemu 1 build-alt          # 使用指定构建目录
pixi run test                      # 运行全部测试（先 test-host，再 test-kernel）
pixi run test-host                 # 构建 build-host/ 并运行带 ASan/UBSan 的 GoogleTest 宿主机单测
pixi run test-host 'FdtTest.*'     # 按 gtest 过滤器仅运行 FdtTest 套件
pixi run test-kernel               # 构建 build-test/ 并在 4 hart QEMU 下运行全部 KTest 用例
pixi run test-kernel 1 mm          # 1 hart 仅运行 mm 测试套件
pixi run disk                      # 单独重建 build/disk.img
```

- `configure` 与 `build` 接受位置参数 `[buildDir] [preset]`（默认 `build qemu-virt`），可按需传入 `qemu-virt-test` 或 `host-test` preset（例如 `pixi run build build-host host-test`）；`disk`、`clean` 接受可选参数 `[buildDir]`（默认 `build`）。
- `test` 汇总执行 `test-host` 与 `test-kernel`；单独执行时：`qemu` 接受 `[harts] [buildDir]`（默认 `1 build`），`test-kernel` 接受 `[harts] [filter] [buildDir]`（默认 `4 all build-test`），`test-host` 接受 `[filter] [buildDir]`（默认 `* build-host`，透传为 `--gtest_filter`）。超过 `SEELE_MAX_HARTS` 的 hart 会在入口停机。
- `configure` 仅在目标目录缺少 `CMakeCache.txt` 时套用对应 preset；修改已有配置需显式调用 `pixi run cmake`。
- `qemu` 与 `test-kernel` 会自动执行 `setup`、`build` 与 `disk`。`disk` 会覆盖目标 `disk.img`，请勿存放持久化数据。
- `pixi run clean <buildDir>` 递归删除目标目录，执行前须确认路径。

| 工具           | 职责                                                        | 入口                                                                           |
| -------------- | ----------------------------------------------------------- | ------------------------------------------------------------------------------ |
| Pixi           | 依赖与任务编排                                              | [pixi.toml](../pixi.toml)                                                      |
| CMake + Ninja  | 编译、汇编、链接 `kernel.elf`，导出 `compile_commands.json` | [CMakeLists.txt](../CMakeLists.txt)、[CMakePresets.json](../CMakePresets.json) |
| TypeScript CLI | 生成 1 MiB 测试磁盘、启动 QEMU                              | [tools/cli.ts](../tools/cli.ts)                                                |

### Release 与 Debug

默认 preset 为 **Release**（`-O2 -DNDEBUG`），所有配置均附带 `-g`。Debug 构建额外定义 `_GLIBCXX_ASSERTIONS`，需配置独立目录：

```bash
pixi run cmake --preset qemu-virt -B build-debug -DCMAKE_BUILD_TYPE=Debug
pixi run build build-debug
pixi run qemu 1 build-debug
```

`SEELE_ASSERT` 在 Release 与 Debug 中均启用，不受 `NDEBUG` 控制。调用栈与地址符号化见 [debug.md](debug.md)。

### QEMU 参数与退出方式

[tools/lib/qemu.ts](../tools/lib/qemu.ts) 默认生成：`virt`、128 MiB RAM、`-bios none`、`-serial stdio -monitor none` 以及一块 1 MiB virtio-blk 测试盘（当前无块设备驱动，仅用于设备枚举），并支持按需附加 `-append`（写入 `/chosen/bootargs`）。[tools/cli.ts](../tools/cli.ts) 的 `test` 子命令额外提供看门狗超时保护（默认 30000 ms），超时仅终止本次启动的 QEMU 子进程并返回退出码 `124`。可通过环境变量 `SEELE_QEMU` 指定 QEMU 二进制路径。

看到 `seele>` 提示符后，运行 `help`、`echo hello`、`info` 完成基线冒烟验证，随后执行 `poweroff` 即可经 `sifive_test` 关机设备正常退出 QEMU。由于串口未启用 multiplexer 且 `Ctrl-C` 仅取消当前输入行，若内核停机或 shell 无响应，可在另一终端确认本次启动的 `qemu-system-riscv64` PID 后执行 `kill -TERM <PID>`。

## 3. 构建参数

所有 `SEELE_*` 参数由 [CMakeLists.txt](../CMakeLists.txt) 定义并通过 `-D` 同步注入 C++ 与汇编，头文件不设默认值以保证布局一致：

| 变量                         | 默认值                            | 含义                                             |
| ---------------------------- | --------------------------------- | ------------------------------------------------ |
| `SEELE_MAX_HARTS`            | 64                                | cpuid、`cpus[]`、hart 描述及启动栈槽位的统一上限 |
| `SEELE_BOOT_STACK_SIZE`      | 65536                             | boot hart 启动栈字节数                           |
| `SEELE_SECONDARY_STACK_SIZE` | 8192                              | 每个 secondary 启动栈字节数                      |
| `SEELE_KSTACK_SIZE`          | 16384                             | 每线程内核栈字节数                               |
| `SEELE_MAX_REGIONS`          | 32                                | 启动内存区域表容量                               |
| `SEELE_MAX_DEVICES`          | 48                                | 平台设备表容量                                   |
| `SEELE_MAX_RESOURCES`        | 128                               | 平台资源表容量                                   |
| `SEELE_MAX_DRIVERS`          | 32                                | 驱动探测顺序表容量                               |
| `SEELE_MAX_IRQ_HANDLERS`     | 16                                | 外部中断 handler 表容量                          |
| `SEELE_FDT_MAX_DEPTH`        | 32                                | FDT 遍历深度上限                                 |
| `SEELE_UART_BAUD`            | 115200                            | UART 目标波特率                                  |
| `SEELE_TESTS`                | `OFF`（注入为 `0` / `1`）         | 是否编译内核自检框架 `KTest` 并在 panic 时退出   |
| `SEELE_RISCV_ARCH`           | `rv64gc`                          | ISA 字符串                                       |
| `SEELE_RISCV_ABI`            | `lp64d`                           | ABI                                              |
| `SEELE_RISCV_CMODEL`         | `medany`                          | 代码模型                                         |
| `SEELE_LINKER_SCRIPT`        | `src/kernel/linker.ld` 的绝对路径 | 链接脚本                                         |

重配示例：

```bash
pixi run cmake --preset qemu-virt -DSEELE_MAX_HARTS=8 -DSEELE_BOOT_STACK_SIZE=32768
pixi run build
```

配置阶段校验所有数值为正整数、三个栈大小均为 4 KiB 倍数、`SEELE_MAX_RESOURCES <= 65535`、`SEELE_UART_BAUD <= 4000000`；源码 `static_assert` 额外校验线程栈窗口至少可容纳 2 个槽位。

## 4. Freestanding 与 ABI 边界

### 编译与链接选项

`-ffreestanding` 仅约束编译器语义，入口、链接脚本、C/C++ 运行时符号与堆均由内核自行提供。

| 选项                                                   | 作用                                                             |
| ------------------------------------------------------ | ---------------------------------------------------------------- |
| C++26（关闭 CMake C++ extensions）                     | 固定语言标准                                                     |
| `-fno-exceptions -fno-rtti`                            | 禁用异常展开与 RTTI                                              |
| `-fno-threadsafe-statics`                              | 关闭局部静态变量并发初始化保护，局部静态对象须在单线程期首次构造 |
| `-fno-use-cxa-atexit`                                  | 避免生成 `__cxa_atexit` 注册调用                                 |
| `-fno-stack-protector -fno-pic -fno-pie`               | 静态裸机镜像，不引入 `__stack_chk_fail`                          |
| `-fno-omit-frame-pointer`、`-g`、关闭 unwind tables    | 支持帧指针栈回溯与地址符号化，无需 `.eh_frame`                   |
| `-ffunction-sections -fdata-sections`、`--gc-sections` | 链接期回收未用段；`.init_array` 由链接脚本 `KEEP` 保留           |
| `-Wall -Wextra -Wpedantic`                             | 开启编译警告（当前未启用 `-Werror`）                             |
| `-nostdlib -nostartfiles -static`                      | 不链接 CRT、宿主库或 libgcc                                      |

`_entry` 固定在 [start.S](../src/kernel/start.S) 中完成选栈后再进入 C++。代码直接使用 Sstc 等 CSR，当前面向 QEMU `virt` 默认 ISA 配置。

### 运行时符号归属

| 符号/能力                                                                | 提供方与约束                                                                                       |
| ------------------------------------------------------------------------ | -------------------------------------------------------------------------------------------------- |
| `.init_array` 边界与调用                                                 | [linker.ld](../src/kernel/linker.ld) 定义边界，[kmain.cc](../src/kernel/kmain.cc) 清 `.bss` 后执行 |
| `std::nothrow`、纯虚/已删除虚函数失败入口、弱 `__cxx_panic`              | [src/cxx/runtime.cc](../src/cxx/runtime.cc)，失败转入 panic                                        |
| `operator new/delete`                                                    | [heap.cc](../src/kernel/mm/heap.cc)，受堆大小类别与对齐限制（见 [memory.md](memory.md)）           |
| `abort`                                                                  | [src/libc/stdlib.cc](../src/libc/stdlib.cc)，转入 panic                                            |
| `memcpy/memmove/memset/memcmp/memchr`、`strlen/strcmp/strncmp`           | [src/libc/string.cc](../src/libc/string.cc)，声明于 [string.h](../src/libc/include/string.h)       |
| `std::terminate` 系列、`malloc/free`、析构注册 (`atexit`/`__cxa_atexit`) | 未提供；避免引入带非平凡析构的静态对象                                                             |

`seele_libc` 用 C++ 实现并以 `extern "C"` 导出，编译时统一启用 `-fno-builtin` 防止自递归优化。

### 标准头文件与编译器 helper 边界

- 已使用 `<array>`、`<atomic>`、`<optional>`、`<span>`、`<string_view>`、`<algorithm>` 等 freestanding 头文件；需注意部分失败路径可能引用 `abort` 或未实现的终止函数。C 字符串接口使用项目自带 `<string.h>` 而非 `<cstring>`。
- `std::atomic` 仅使用目标原生 lock-free（至多 8 字节）操作，无 libatomic 兜底。
- 128 位除法或宽原子等可能生成 libgcc/compiler-rt helper 的操作须经符号检查确认。
- M-mode 启用 FPU（`mstatus.FS`），但内核线程切换暂不保存/恢复浮点寄存器。

## 5. 新增运行时依赖验证流程

1. 明确新增符号归属（头文件、libc、C++ ABI 或编译器 helper）；新增 libc 源文件须加入 CMake `seele_libc` 目标。
2. 在内核实际调用路径中编译并覆盖失败分支。
3. 构建 Release 与 Debug，核对最终 ELF 无未定义符号（两次 `nm -u` 均应无输出）：

    ```bash
    pixi run build
    pixi run riscv64-unknown-elf-nm -u build/kernel.elf
    pixi run build build-debug
    pixi run riscv64-unknown-elf-nm -u build-debug/kernel.elf
    ```

4. 启动 QEMU 验证实际运行路径；若涉及自定义链接段，同步核对 [linker.ld](../src/kernel/linker.ld) 的保留规则。
