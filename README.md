# seele-os

面向 QEMU `virt` 的 RISC-V 64 位实验内核，使用 C++26 与 GCC 16。内核自行完成 M-mode 到 S-mode 的引导（不依赖 OpenSBI），采用 freestanding 构建，不链接宿主 libc、libstdc++ 或 CRT。

当前支持单 hart 与多 hart 启动、Sv39 分页与内核堆、设备树发现与驱动探测，以及运行在独立 U-mode 用户进程中的交互式 shell（独立编译后经 C++26 `#embed` 嵌入内核，通过 `read`/`write`/`exit`/`shutdown` 系统调用与内核交互）。项目仍处于基础机制演进阶段，尚无文件系统、多进程管理与稳定外部 ABI。

## 快速开始

### 准备环境

- 开发环境：Linux x86-64（宿主机 glibc ≥ 2.28；Pixi workspace 仅声明 `linux-64`）。
- 安装 Pixi（版本要求见 [pixi.toml](pixi.toml)，当前 `>=0.84`）；其余依赖及来自 `https://prefix.dev/seele-official/seele-os-toolchain` 的 RISC-V 交叉工具链均由 Pixi 管理。

在项目根目录运行：

```bash
pixi install
pixi run qemu
```

`qemu` 会自动安装脚本依赖、构建 `build/kernel.elf`、重建测试磁盘并启动 QEMU。**每次运行都会覆盖对应构建目录的 `disk.img`，请勿在其中保存数据。** 工具链更新与源码重建见 [构建与运行](docs/build.md)。

### 验证启动

串口输出启动日志后会出现：

```text
seele-os shell: type 'help' for commands
seele>
```

依次运行 `help`、`echo hello` 验证命令分发与输入回显，运行 `poweroff` 经 `shutdown` 系统调用正常关机退出 QEMU。shell 仅支持可打印 ASCII（单行最多 127 字符）、退格与 `Ctrl-C` 取消当前行；更多运行与测试说明见 [构建与运行](docs/build.md)。

### 常用命令

| 目的                           | 命令                           |
| ------------------------------ | ------------------------------ |
| 仅构建内核                     | `pixi run build`               |
| 运行全部测试（宿主机 + 内核）  | `pixi run test`                |
| 仅运行宿主机单测（ASan/UBSan） | `pixi run test-host`           |
| 仅运行内核自检（4 hart QEMU）  | `pixi run test-kernel`         |
| 4 hart 启动 QEMU               | `pixi run qemu 4`              |
| 指定构建目录启动               | `pixi run qemu 1 build-alt`    |
| 配置 Debug 构建                | 见 [构建与运行](docs/build.md) |

`qemu` 参数顺序为 `[harts] [buildDir]`（默认 `1 build`），默认使用 Release preset、QEMU `virt`、128 MiB RAM、`-bios none` 及 1 MiB virtio-blk 测试盘。

## 已实现与边界

| 子系统     | 当前能力                                                                         | 主要边界                                                     |
| ---------- | -------------------------------------------------------------------------------- | ------------------------------------------------------------ |
| 启动       | M-mode 初始化、S-mode 交接、稠密 cpuid、多 hart 启动                             | boot hart 固定为硬件 hart 0；仅面向 QEMU `virt`              |
| 平台与 I/O | FDT 解析、设备枚举、驱动探测、PLIC、16550 UART、关机设备                         | 外部中断仅由 boot hart 处理；尚无 virtio 驱动                |
| 内存       | 物理页分配、Sv39、每进程用户空间、共享内核栈窗口、`new`/`delete`                 | 无按需分页或跨 hart TLB shootdown；堆大小类别为 16–1024 字节 |
| 线程与用户 | 系统进程与 U-mode 用户进程、ELF64 加载、基础系统调用（read/write/exit/shutdown） | 无动态进程回收（reaper/`wait`）、优先级或跨 hart 调度迁移    |
| 诊断与测试 | 格式化日志、assert/panic、帧指针调用栈、可选内核自检                             | 无日志级别过滤；默认构建 panic 后停机不恢复                  |

## 文档导航

首次阅读建议按“构建 → 架构 → 相关子系统”的顺序；修改代码前请先阅读开发约定。

| 文档                             | 涵盖内容                                               |
| -------------------------------- | ------------------------------------------------------ |
| [构建与运行](docs/build.md)      | 环境准备、Debug 构建、构建参数、工具链更新与运行时边界 |
| [系统架构](docs/architecture.md) | 启动顺序、模块边界、hart 模型与跨模块不变量            |
| [内存管理](docs/memory.md)       | 地址类型、物理页、Sv39、堆与内核栈                     |
| [平台与设备](docs/platform.md)   | FDT、设备资源、驱动探测、MMIO、中断与 console          |
| [线程与调度](docs/scheduling.md) | 进程/线程、上下文切换、trap、睡眠与唤醒                |
| [调试与诊断](docs/debug.md)      | 日志、panic、调用栈与地址符号化                        |
| [开发约定](docs/conventions.md)  | 命名、修饰符、所有权、错误处理、注释与提交检查         |

## 源码入口

- [src/kernel/kmain.cc](src/kernel/kmain.cc)：S-mode 启动主线；M-mode 入口位于 [src/kernel/boot/](src/kernel/boot/)（[start.S](src/kernel/boot/start.S) 与 [start.cc](src/kernel/boot/start.cc)）。
- [src/kernel/mm/](src/kernel/mm/)：物理页、页表与堆。
- [src/kernel/platform/](src/kernel/platform/) 与 [src/kernel/drivers/](src/kernel/drivers/)：设备模型与驱动。
- [src/kernel/proc/](src/kernel/proc/) 与 [src/kernel/sched/](src/kernel/sched/)：进程、线程、ELF64 加载、系统调用与调度。
- [src/uapi/](src/uapi/) 与 [src/user/](src/user/)：内核/用户共享系统调用 UAPI、用户态 CRT 与独立用户态 shell。
- [src/util/](src/util/) 与 [src/libc/](src/libc/)：内核与用户态共享的 freestanding 格式化/编译期字符串工具与 C 字符串库；内核 C++ 运行时与 `operator new/delete` 位于 [src/kernel/cxx/](src/kernel/cxx/)。
- [tools/](tools/)：测试磁盘与 QEMU 启动脚本；[CMakeLists.txt](CMakeLists.txt)、[cmake/](cmake/) 与各模块 `CMakeLists.txt` 负责内核、用户态与宿主机测试构建。
