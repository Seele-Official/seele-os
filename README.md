# seele-os

面向 QEMU `virt` 的 RISC-V 64 位实验内核，使用 C++26 和 GCC 16。内核从 M-mode 自行进入 S-mode，不依赖 OpenSBI，也不链接宿主 libc、libstdc++ 或 CRT。

目前可以在单 hart 或多 hart 下启动，完成内存与设备初始化，并在 boot hart 上运行内核 shell。项目仍处于内核机制验证阶段，**不是可日常使用的操作系统，也没有稳定的外部接口**。

## 快速开始

### 准备环境

- 开发环境为 Linux x86-64（Pixi workspace 目前只声明 `linux-64`）。
- 安装 Pixi，版本要求见 [pixi.toml](pixi.toml)（当前为 `>=0.84`）；其余工具由 Pixi 管理。
- 将独立的 `seele-toolchain` 仓库准备在项目根目录下的 `toolchain/`。它不随本仓库提供，也不是自动拉取的子模块；缺少它时，下面的本地 channel 构建无法执行。

在项目根目录运行：

```bash
# 首次使用：在独立工具链仓库中组装本地包 channel
(cd toolchain && pixi run channel)

# 安装项目依赖，构建并启动单 hart 内核
pixi install
pixi run qemu
```

`qemu` 任务会自动安装脚本依赖、构建 `build/kernel.elf`、重建测试磁盘，再启动 QEMU。**每次运行都会覆盖对应构建目录的 `disk.img`，不要用它保存数据。**
若 lock 文件中的本地 channel 路径来自其他机器，先按[构建文档](docs/build.md)重新解析工具链依赖。

### 验证启动

串口输出启动日志后应出现：

```text
seele-os shell: type 'help' for commands
seele>
```

输入 `help` 查看命令，`echo hello` 验证输入与回显，`info` 查看当前线程、进程和内核栈。这只是内核测试 shell：没有文件系统命令或用户程序。输入仅支持可打印 ASCII，一行最多 127 个字符；支持退格，收到 `Ctrl-C` 字节时取消当前行。当前 shell 没有 `exit` 命令，启动脚本也未启用 `Ctrl-A X` 退出快捷键；结束模拟器的方法见[构建文档](docs/build.md)。

### 常用命令

| 目的               | 命令                          |
| ------------------ | ----------------------------- |
| 只构建，不启动     | `pixi run build`              |
| 用 4 个 hart 启动  | `pixi run qemu 4`             |
| 使用另一个构建目录 | `pixi run qemu 1 build-alt`   |
| 配置 Debug 构建    | 见[构建与运行](docs/build.md) |

`qemu` 参数顺序为 `[harts] [buildDir]`，默认 `1 build`。新的构建目录默认使用 Release preset；Debug 配置步骤见构建文档。QEMU 默认使用 `virt`、128 MiB 内存和 `-bios none`，附带的 1 MiB virtio-blk 磁盘目前仅用于设备枚举。

## 已实现与边界

| 子系统     | 当前能力                                                   | 主要边界                                                        |
| ---------- | ---------------------------------------------------------- | --------------------------------------------------------------- |
| 启动       | M-mode 初始化、S-mode 交接、稠密 cpuid、多 hart 启动       | boot hart 固定为硬件 hart 0；仅面向 QEMU `virt`                 |
| 平台与 I/O | FDT 解析、设备枚举、驱动探测、PLIC、16550 UART             | 外部中断只在 boot hart 处理；尚无 virtio 驱动                   |
| 内存       | 物理页分配、Sv39、进程根页表、每线程内核栈、`new`/`delete` | 无用户映射、缺页处理或 TLB shootdown；堆大小类别为 16–1024 字节 |
| 线程       | 每 hart 的 idle、boot hart 的 shell、就绪/睡眠队列与唤醒   | 无运行时线程创建/退出、优先级或跨 hart 调度                     |
| 诊断       | 格式化日志、assert/panic、帧指针调用栈                     | 无日志级别过滤；panic 后不恢复                                  |

shell 只在控制台已挂接时创建。UART 优先使用中断收发，无可用中断线时回退轮询；输入缓冲有限且没有流控，不应把启动前粘贴大量文本当作可靠输入方式。详细限制分别记录在对应的专题文档中。

## 文档导航

第一次阅读建议按“构建 → 架构 → 相关子系统”的顺序；修改代码前再看开发约定。

| 文档                             | 适合解决的问题                                         |
| -------------------------------- | ------------------------------------------------------ |
| [构建与运行](docs/build.md)      | 环境准备、Debug 构建、构建参数、工具链更新与运行时依赖 |
| [系统架构](docs/architecture.md) | 启动顺序、模块边界、hart 模型与跨模块不变量            |
| [内存管理](docs/memory.md)       | 地址类型、物理页、Sv39、堆与内核栈                     |
| [平台与设备](docs/platform.md)   | FDT、设备资源、驱动探测、MMIO、中断与 console          |
| [线程与调度](docs/scheduling.md) | 进程/线程、上下文切换、trap、睡眠与唤醒                |
| [调试与诊断](docs/debug.md)      | 日志、panic、调用栈和地址符号化                        |
| [开发约定](docs/conventions.md)  | 命名、所有权、错误处理、注释与提交检查                 |

## 源码入口

- [src/kernel/kmain.cc](src/kernel/kmain.cc)：S-mode 启动主线；M-mode 入口为 [start.S](src/kernel/start.S) 和 [start.cc](src/kernel/start.cc)。
- [src/kernel/mm/](src/kernel/mm/)：物理页、页表和堆。
- [src/kernel/platform/](src/kernel/platform/) 与 [drivers/](src/kernel/drivers/)：设备模型和驱动。
- [src/kernel/proc/](src/kernel/proc/) 与 [shed/](src/kernel/shed/)：进程、线程和调度；
- [src/libc/](src/libc/) 与 [src/cxx/runtime.cc](src/cxx/runtime.cc)：项目自己的 C 库与 C++ ABI 支持。
- [tools/](tools/)：测试磁盘与 QEMU 启动脚本；[CMakeLists.txt](CMakeLists.txt) 只负责构建内核。
