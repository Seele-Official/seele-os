# 构建与运行

本文说明如何准备工具链、选择构建配置和运行 QEMU，以及新增 C/C++ 依赖时必须遵守的
freestanding 边界。首次启动的最短流程见 [README](../README.md)。

## 1. 准备工具链

本仓库通过 [pixi.toml](../pixi.toml) 管理依赖，目前只支持 `linux-64` 开发环境，要求
Pixi `>=0.84`。RISC-V 工具链来自项目根目录下的独立仓库 `toolchain/`
（`seele-toolchain`）；它不纳入本仓库，需要另外准备。

```bash
# 从项目根目录执行；toolchain/ 必须已经存在
(cd toolchain && pixi run channel)
pixi install
```

本地 channel 提供两个固定版本包：

| 包                              | 当前锁定版本 | 用途                    |
| ------------------------------- | ------------ | ----------------------- |
| `riscv64-unknown-elf-gcc`       | 16.2.0       | 交叉编译器和 GNU 工具   |
| `riscv64-unknown-elf-libstdcxx` | 16.2.0       | freestanding C++ 头文件 |

CMake 按名字查找 `riscv64-unknown-elf-gcc/g++`；通过 `pixi run` 执行命令即可使用项目
环境，不需要自行改宿主 `PATH`。工具链包是否含有某个库，与内核是否链接该库是两回事：
当前内核不链接宿主 libc、libstdc++、libsupc++、libgcc 或 compiler-rt。

### 更新工具链或迁移工作目录

工具链仓库生成的 channel 位于 `toolchain/.channel`。修改工具链内容后先重新生成
channel；`pixi.lock` 中的本地 `file://` 包地址含绝对路径，换机器或移动目录后也需要
重新解析相关依赖，不能只复制旧环境：

```bash
(cd toolchain && pixi run channel)
pixi update riscv64-unknown-elf-gcc riscv64-unknown-elf-libstdcxx
pixi install
```

检查 lock 差异，确认工具链包地址指向当前工作目录；不要为修复本地路径而顺带升级所有
依赖。工具链获取与打包方式以独立仓库自身的说明为准。

## 2. 构建与运行

### 常用任务

以下命令均在项目根目录运行：

```bash
pixi run build                    # 配置并构建 build/kernel.elf
pixi run qemu                     # 构建、准备磁盘并启动：1 hart，build/
pixi run qemu 4                   # 同上，4 个 hart
pixi run qemu 1 build-alt         # 使用另一个构建目录
pixi run disk                    # 单独准备 build/disk.img
```

- `configure`、`build`、`disk`、`clean` 接受一个可选参数 `[buildDir]`，默认 `build`。
- `qemu` 接受位置参数 `[harts] [buildDir]`，默认 `1 build`；hart 数应不超过构建时的
  `SEELE_MAX_HARTS`，超过上限的 hart 会在入口停住。
- `configure` 只在目标目录没有 `CMakeCache.txt` 时使用 `qemu-virt` preset。
  **修改已有配置必须显式运行 CMake**，重复执行 `pixi run configure` 不会重配。
- `pixi run qemu` 自动运行 `setup`、`build` 和 `disk`；单独运行 `build` 不会生成磁盘。
  `disk` 以写入模式重建目标 `disk.img`，会清除其原有内容；不要在该测试镜像中保存数据。
- `pixi run clean [buildDir]` 会递归删除整个目标目录，执行前确认参数，不要传项目根目录。

职责与入口：

| 工具           | 职责                                                        | 配置/实现                                                           |
| -------------- | ----------------------------------------------------------- | ------------------------------------------------------------------- |
| Pixi           | 依赖与任务编排                                              | [pixi.toml](../pixi.toml)                                           |
| CMake + Ninja  | 编译、汇编、链接 `kernel.elf`，导出 `compile_commands.json` | [CMakeLists.txt](../CMakeLists.txt)、[preset](../CMakePresets.json) |
| TypeScript CLI | 生成 1 MiB 测试磁盘、启动 QEMU                              | [tools/cli.ts](../tools/cli.ts)                                     |

### Release 与 Debug

默认 preset 是 **Release**（`-O2 -DNDEBUG`），所有配置都带 `-g`。
需要 Debug 时，先显式配置独立目录，再构建和运行：

```bash
pixi run cmake --preset qemu-virt -B build-debug -DCMAKE_BUILD_TYPE=Debug
pixi run build build-debug
pixi run qemu 1 build-debug
```

Debug 额外定义 `_GLIBCXX_ASSERTIONS`。项目自己的 `SEELE_ASSERT` 在 Release 中也启用，
不受 `NDEBUG` 控制。栈回溯和地址符号化见[调试文档](debug.md)。

### QEMU 与启动检查

默认参数由 [tools/lib/qemu.ts](../tools/lib/qemu.ts) 生成：`virt`、128 MiB RAM、
`-bios none`、串口接 stdio、关闭 monitor，并挂接一块 virtio-blk 测试磁盘。内核目前
没有块设备驱动，生成 `disk.img` 不意味着能够访问文件系统。

看到 `seele>` 后依次运行 `help`、`echo hello` 和 `info`，确认命令处理、回显和当前线程
信息。shell 收到 `Ctrl-C` 字节时只取消当前输入行，没有 `exit` 或关机命令。
当前脚本使用 `-serial stdio -monitor none`，未启用串口 multiplexer，不能依赖常见的
`Ctrl-A X` 退出快捷键。需要结束时，在另一终端找到此次启动的 `qemu-system-riscv64`
进程 PID，对该进程执行 `kill -TERM <PID>`；先确认 PID，避免终止其他实例。
如需指定模拟器二进制，可设置 `SEELE_QEMU=/path/to/qemu-system-riscv64`。

## 3. 修改构建参数

构建参数由 CMake cache 定义，并通过 `-D` 同时传给 C++ 与汇编。头文件不提供默认回退，
避免汇编与 C++ 使用不同布局。默认值与校验以 [CMakeLists.txt](../CMakeLists.txt) 为准。

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
| `SEELE_MAX_IRQ_HANDLERS`     | 16                                | 外部中断 handler 表容量，并非控制器的中断源总数  |
| `SEELE_FDT_MAX_DEPTH`        | 32                                | FDT 遍历深度上限                                 |
| `SEELE_UART_BAUD`            | 115200                            | UART 目标波特率                                  |
| `SEELE_RISCV_ARCH`           | `rv64gc`                          | ISA 字符串                                       |
| `SEELE_RISCV_ABI`            | `lp64d`                           | ABI                                              |
| `SEELE_RISCV_CMODEL`         | `medany`                          | 代码模型                                         |
| `SEELE_LINKER_SCRIPT`        | `src/kernel/linker.ld` 的绝对路径 | 链接脚本                                         |

例如，重新配置默认目录并构建：

```bash
pixi run cmake --preset qemu-virt -DSEELE_MAX_HARTS=8 -DSEELE_BOOT_STACK_SIZE=32768
pixi run build
```

配置阶段检查正整数、三个栈大小均为 4 KiB 的倍数、`SEELE_MAX_HARTS >= 2`、boot 栈
大于 secondary 栈、`SEELE_MAX_RESOURCES <= 65535` 和 `SEELE_UART_BAUD <= 4000000`。
`SEELE_MAX_HARTS >= 2` 是栈池的构建约束，不妨碍 QEMU 只启动一个 hart。内核栈是否能
在窗口中容纳至少两个线程，还受源码 `static_assert` 约束。

旧参数 `SEELE_BOOT_STACK_HARTS` 已合并进 `SEELE_MAX_HARTS`，残留 cache 会导致配置
失败，可用 `pixi run cmake --preset qemu-virt -U SEELE_BOOT_STACK_HARTS` 清除。
这些检查只验证构建约束，不代表任意 ISA、ABI、波特率或链接布局都经过运行测试。

## 4. freestanding 与 ABI 边界

### 编译和链接分别保证什么

`-ffreestanding` 选择编译语义，**不会提供启动代码或运行时**。内核自行提供入口、链接
脚本、C 库符号、必要的 C++ ABI 支持和堆；C++ 头文件由工具链包提供。

| 设置                                        | 目的与边界                                               |
| ------------------------------------------- | -------------------------------------------------------- |
| C++26、关闭 CMake C++ extensions            | 固定语言模式；不是“源码完全不使用编译器扩展”的保证       |
| `-fno-exceptions -fno-rtti`                 | 不使用异常展开或 RTTI                                    |
| `-fno-threadsafe-statics`                   | 不生成线程安全局部静态初始化保护；不能依赖并发首次初始化 |
| `-fno-use-cxa-atexit`                       | 改变静态析构注册方式，不能消除对终止运行时的需求         |
| `-fno-stack-protector`                      | 不引入 `__stack_chk_fail`                                |
| `-fno-pic -fno-pie`                         | 静态裸机镜像                                             |
| `-fno-omit-frame-pointer`、`-g`             | 支持帧指针回溯和地址符号化，不保证每个源函数都有独立栈帧 |
| 关闭 unwind 表、启用 function/data sections | 不依赖异常展开，允许链接时回收未使用段                   |
| `-Wall -Wextra -Wpedantic`                  | 开启警告；当前没有 `-Werror`                             |
| `-nostdlib -nostartfiles -static`           | 不自动链接 CRT、C/C++ 库或 libgcc                        |
| 自定义链接脚本、`--gc-sections`             | 决定入口、段布局及保留范围；`.init_array` 使用 `KEEP`    |

`_entry` 保持在 [start.S](../src/kernel/start.S) 中：选栈发生在任何 C++ 调用之前，不能
依赖编译器“恰好不生成栈访问”。当前代码直接使用 Sstc 等 CSR；只改 ISA 字符串并不足以
移植到缺少这些硬件能力的平台。

### 符号由谁提供

| 能力                                                                      | 提供方与限制                                                                                              |
| ------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------- |
| `.init_array` 边界与调用                                                  | [链接脚本](../src/kernel/linker.ld) 提供边界；[kmain.cc](../src/kernel/kmain.cc) 清 `.bss` 后执行构造函数 |
| `std::nothrow`、纯虚/已删除虚函数失败入口、弱 `__cxx_panic`               | [src/cxx/runtime.cc](../src/cxx/runtime.cc)；失败入口接到 panic                                           |
| `std::terminate`、`set_terminate`、`get_terminate`、`uncaught_exceptions` | 当前只有声明，没有实现；不能依赖它们                                                                      |
| 可替换的 `operator new/delete`                                            | [mm/heap.cc](../src/kernel/mm/heap.cc)；受内核堆大小与对齐契约限制                                        |
| `abort`                                                                   | [src/libc/stdlib.cc](../src/libc/stdlib.cc)，接到 panic                                                   |
| `memcpy/memmove/memset/memcmp/memchr`、`strlen/strcmp/strncmp`            | [src/libc/string.cc](../src/libc/string.cc)，由项目 [string.h](../src/libc/include/string.h) 声明         |
| `malloc/free/calloc/realloc`、终止与析构注册函数                          | 未提供；不要把 `new` 的可用性当作 C 堆或完整 C++ 运行时可用                                               |

libc 用 C++ 实现，以 `extern "C"` 导出。CMake 为若干 libc 函数单独设置
`-fno-builtin-*`，并不是对整个库使用通用 `-fno-builtin`。新增实现时检查这份选项列表，
避免编译器把函数体优化成对自身的递归调用。

当前避免带非平凡析构的静态对象。引入此类对象可能增加 `atexit`、`__cxa_atexit` 或
`__dso_handle` 依赖，必须明确生命周期，而不是随意补空实现以消除链接错误。

### 使用 C++ 头文件

内核已使用 `<array>`、`<atomic>`、`<optional>`、`<span>`、`<string_view>`、
`<algorithm>` 等 freestanding 设施，但“头文件能包含”不等于“所有成员调用都能链接”。
有些失败路径仍会引用 `abort` 或尚未实现的终止函数。不要仅凭 hosted 环境的经验引入
`<string>`、`<iostream>` 等设施；`<cstring>` 也不能替代项目自己的 `<string.h>`。

- atomic 只依赖当前目标上 lock-free、至多 8 字节的操作；没有 libatomic 兜底。
- 128 位整数除法、部分位运算或更宽 atomic 可能生成编译器 helper；当前不链接 libgcc/
  compiler-rt/libatomic，必须逐项确认。
- 进入 M-mode 时启用 FPU，不代表线程支持浮点：当前没有浮点寄存器上下文保存/恢复。

## 5. 新增运行时依赖的检查流程

1. 确认依赖属于头文件、C 库、C++ ABI 还是编译器 helper，不把所有未定义符号都归到 libc。
2. 在实际内核目标中编译并调用需要的接口，覆盖可能生成额外符号的失败路径。
3. 新增 libc 源文件时登记到 CMake 的 `seele_libc`，并检查所需 `-fno-builtin-*`。
4. 构建 Release 和 Debug，检查最终 ELF 的未定义符号：

    ```bash
    pixi run build
    pixi run riscv64-unknown-elf-nm -u build/kernel.elf
    pixi run build build-debug
    pixi run riscv64-unknown-elf-nm -u build-debug/kernel.elf
    ```

    Debug 目录应事先按上文配置。当前预期两次 `nm -u` 均无输出；这只说明没有未解析的
    外部符号，不证明 ABI、初始化顺序或运行行为正确。

5. 启动 QEMU 验证受影响路径；涉及静态构造表或其他注册段时，检查链接脚本是否保留了
   所需段。复核日志、panic 和调用栈的操作见[调试文档](debug.md)。
