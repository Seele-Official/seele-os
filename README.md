# seele-os

面向 QEMU `virt` 的 RISC-V 64 位实验内核，使用 C++26 与 GCC 16。内核自行完成 M-mode 到 S-mode 的引导（不依赖 OpenSBI），采用 freestanding 构建，不链接宿主 libc、libstdc++ 或 CRT。

当前支持单 hart 与多 hart 启动、Sv39 分页与内核堆、设备树发现与驱动探测，以及 boot hart 上的交互式内核 shell。项目处于内核机制验证阶段，尚无用户态与稳定外部接口。

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

依次运行 `help`、`echo hello`、`info` 验证命令分发、输入回显和线程信息。shell 仅支持可打印 ASCII（单行最多 127 字符）、退格与 `Ctrl-C` 取消当前行；退出 QEMU 的方法见 [构建与运行](docs/build.md)。

### 常用命令

| 目的             | 命令                           |
| ---------------- | ------------------------------ |
| 仅构建内核       | `pixi run build`               |
| 4 hart 启动 QEMU | `pixi run qemu 4`              |
| 指定构建目录启动 | `pixi run qemu 1 build-alt`    |
| 配置 Debug 构建  | 见 [构建与运行](docs/build.md) |

`qemu` 参数顺序为 `[harts] [buildDir]`（默认 `1 build`），默认使用 Release preset、QEMU `virt`、128 MiB RAM、`-bios none` 及 1 MiB virtio-blk 测试盘。

## 已实现与边界

| 子系统     | 当前能力                                                   | 主要边界                                                        |
| ---------- | ---------------------------------------------------------- | --------------------------------------------------------------- |
| 启动       | M-mode 初始化、S-mode 交接、稠密 cpuid、多 hart 启动       | boot hart 固定为硬件 hart 0；仅面向 QEMU `virt`                 |
| 平台与 I/O | FDT 解析、设备枚举、驱动探测、PLIC、16550 UART             | 外部中断仅由 boot hart 处理；尚无 virtio 驱动                   |
| 内存       | 物理页分配、Sv39、进程根页表、每线程内核栈、`new`/`delete` | 无用户映射、缺页处理或 TLB shootdown；堆大小类别为 16–1024 字节 |
| 线程       | 每 hart 的 idle、boot hart 的 shell、就绪/睡眠队列与唤醒   | 无运行时线程创建/退出、优先级或跨 hart 调度                     |
| 诊断       | 格式化日志、assert/panic、帧指针调用栈                     | 无日志级别过滤；panic 后停机不恢复                              |

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

- [src/kernel/kmain.cc](src/kernel/kmain.cc)：S-mode 启动主线；M-mode 入口为 [start.S](src/kernel/start.S) 与 [start.cc](src/kernel/start.cc)。
- [src/kernel/mm/](src/kernel/mm/)：物理页、页表与堆。
- [src/kernel/platform/](src/kernel/platform/) 与 [src/kernel/drivers/](src/kernel/drivers/)：设备模型与驱动。
- [src/kernel/proc/](src/kernel/proc/) 与 [src/kernel/sched/](src/kernel/sched/)：进程、线程与调度。
- [src/libc/](src/libc/) 与 [src/cxx/runtime.cc](src/cxx/runtime.cc)：freestanding C 库与 C++ ABI 支持。
- [tools/](tools/)：测试磁盘与 QEMU 启动脚本；[CMakeLists.txt](CMakeLists.txt) 负责内核构建。
