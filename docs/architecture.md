# 系统架构

本文说明启动路径、模块边界、hart 模型与跨模块不变量。构建与运行流程见 [build.md](build.md)，各子系统细节分别维护在对应专题文档中。

## 设计原则

seele-os 当前面向 QEMU `virt` 的 S-mode 运行环境，采用以下设计：

- **自主完成 M-mode 引导**：QEMU 以 `-bios none` 启动，内核自行配置 PMP、trap 委托、计数器访问与 Sstc，随后经 `mret` 进入 S-mode。
- **freestanding 运行时**：不链接宿主 libc、libstdc++ 或 CRT，仅使用工具链提供的 freestanding 头文件与仓库自带的运行时符号。
- **启动期构建静态拓扑**：内存区域、设备、进程与初始线程均在启动期建立；运行期仅允许内核堆按需增长。
- **固定容量与显式失败**：平台表采用构建期固定容量；发现阶段超限时告警并截断，注册接口返回失败，页表或线程栈等关键资源耗尽时 panic。

## 模块分层

```text
boot/start.S → boot/start.cc          M-mode：选栈、cpuid、机器状态
                │ mret
                ▼
kmain.cc                              S-mode 启动编排
  ├─ fdt                              只读设备树解析
  ├─ platform                         早期 console、内存/hart/设备发现、MMIO
  ├─ mm                               物理页 → 页表 → 堆与线程栈
  ├─ drivers                          控制器与设备 probe、正式 console
  └─ proc + sched + trap              线程入口、切换、中断与等待

console + panic + stacktrace          启动与运行期输出与诊断
libc + cxx                            freestanding 运行时支持
```

### 层间契约

| 边界               | 约定                                                                                                   |
| ------------------ | ------------------------------------------------------------------------------------------------------ |
| FDT → 平台         | `fdt` 只做只读解析；`platform/device` 将节点转为 `Device` 与资源，驱动不直接包含 `fdt.h`               |
| 平台 → VM          | `vm::k::init()` 接收设备 span 并映射其 MMIO 资源，不参与 probe 或设备策略                              |
| 控制器/设备 → trap | 控制器实现 `ExternalController` 的 claim/complete，设备注册 `ExternalHandler`；trap 不依赖具体硬件类型 |
| 驱动 → console     | probe 后挂接 `CharDevice`；console 经抽象接口读写，UART 中断处理器不回调 console                       |
| 驱动 → power       | probe 后挂接 `PowerDevice`；`power::poweroff()` 先排空 console 再关机，测试态 panic 走无锁紧急关机     |
| 内核 → 运行时      | `libc` 与 `cxx` 提供 C/C++ 运行时与 ABI 符号，失败入口统一转入 panic                                   |

核心接口定义见 [device.h](../src/kernel/platform/device.h)、[char_device.h](../src/kernel/char_device.h)、[power.h](../src/kernel/power.h)、[trap.h](../src/kernel/trap.h)；生命周期与符号边界分别见 [platform.md](platform.md) 与 [build.md](build.md)。

## 启动顺序

启动编排入口为 [kmain.cc](../src/kernel/kmain.cc)，其中分页与堆的先后顺序固定。

### boot hart

| 阶段           | 动作                                                                                                                                              | 完成后满足的前置条件                                 |
| -------------- | ------------------------------------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------- |
| 汇编入口       | [boot/start.S](../src/kernel/boot/start.S) 分配 boot 栈，将 `cpuid = 0` 写入 `tp`                                                                 | 具备可用栈，全局 C++ 对象尚未构造                    |
| M-mode 交接    | [boot/start.cc](../src/kernel/boot/start.cc) 配置机器状态，以 `satp = 0` 进入 S-mode                                                              | 可在无分页状态下访问物理 RAM 与 MMIO                 |
| 早期发现       | 清 `.bss`、`cxx::init()` 执行 `.init_array`，`platform::init()` 绑定 DTB 并挂接 early console                                                     | DTB 可查询；early console 就绪时可输出后续日志       |
| 平台描述       | `platform::init()` 枚举设备并解析内存/保留区、hart、timebase 与 `/chosen`                                                                         | 平台拓扑与设备资源表只读就绪                         |
| 分页准备       | `phymem::init()`、`vm::k::init()`                                                                                                                 | 物理页可分配，共享内核根表（含堆与内核栈子树）已建立 |
| 启用分页       | `vm::k::enable(vm::k::pagetable())`                                                                                                               | 高位内核堆与共享线程栈窗口可访问                     |
| 内核对象       | `proc::init()` 建立全局唯一系统进程，创建 boot hart 的 `idle` 线程并分配其内核栈槽                                                                | 具备系统进程与可切入的线程上下文                     |
| 驱动与入口线程 | 按级别 probe 驱动；默认构建在 `char_device::active() != nullptr` 时加载 `#embed` 的 ELF64 `shell` 并入队 U-mode 线程，测试构建入队 `ktest` 运行器 | 外部中断与关机设备就绪，初始线程集固定               |
| 发布与切换     | release 发布 `g_started`，单向切入 idle                                                                                                           | secondary 可继续初始化；boot 上下文不再恢复          |

`idle()` 在自身线程栈上归还本 hart 的启动栈，安装 `stvec`、配置定时器并开启中断，随后循环 `wfi`。早期 console 挂接失败不影响启动（挂接前输出丢弃），无效 DTB 则直接断言失败。

### secondary hart

secondary 在汇编入口原子取得稠密 `cpuid` 与启动栈，完成 M-mode 交接后 acquire 等待 `g_started`。读到发布结果后，直接调用 `vm::k::enable(vm::k::pagetable())` 启用共享内核根表，在全局唯一系统进程 `proc::kernel_proc()` 内创建各自的 `idle` 线程（`SEELE_TESTS` 构建下额外入队本 hart 的 `ktest` 工作线程）并切入 `idle()`；不重复平台发现、页表克隆、驱动 probe 或创建 shell。

## hart 与进程/线程模型

- **稠密 `cpuid` 索引**：硬件 `hartid` 仅用于固件交互；boot hart 固定为硬件 hart 0（`cpuid = 0`），secondary 依次原子分配 `1, 2, ...` 并存入 `tp`。`cpus[]`、锁所有者与启动栈池均以 `cpuid` 索引。
- **系统进程与用户进程**：所有 hart 上的内核线程（各 hart 的 `idle` 及测试线程）隶属静态存储中的系统进程 `proc::kernel_proc()`，直接运行在 `vm::k::pagetable()` 上；用户进程（如嵌入内核的 U-mode `shell`）通过 `vm::k::create_process_pagetable()` 派生独立根页表（共享冻结内核根项，独享 Sv39 根项 128–255 用户空间）。
- **每 hart 本地队列**：`ready` 与 `sleeping` 均为 hart 本地队列；`idle` 仅在无可运行线程时兜底，不加入 `ready`。
- **固定容量上限**：`SEELE_MAX_HARTS` 同时限制 hart 描述表、`cpus[]` 与启动栈池；超限 hart 在入口处停机。

## 跨模块不变量

| 不变量                                                       | 原因与约束                                                                                                                                           |
| ------------------------------------------------------------ | ---------------------------------------------------------------------------------------------------------------------------------------------------- |
| 共享内核根项在 `vm::k::init()` 后冻结                        | 派生进程根表复制根项 0–127、256–262 与 508，保留 128–255 为进程私有用户空间；运行期堆与内核栈仅在已预留的共享子树内扩展（见 [memory.md](memory.md)） |
| 线程内核栈位于共享根项 508 子树，每线程独占全局唯一槽位      | 跨 hart、跨进程与中断上下文均可直接寻址栈上等待节点，槽位下方保留未映射 guard 页                                                                     |
| 高位堆仅在 `vm::k::enable()` 后访问                          | 启用分页前不能触发 `new` 或解引用堆对象                                                                                                              |
| DTB 与 `platform` 持有的静态 `fdt::Tree` 存活至内核结束      | 设备名、属性及 `string_view` 均借用该存储                                                                                                            |
| boot 上下文切入 `idle()` 为单向交接                          | `idle()` 会释放本 hart 的启动栈页，旧上下文不得恢复                                                                                                  |
| 外部中断控制器与 handler 注册早于 `g_started` 发布与中断开启 | 运行期分发表只读，无并发注销或热插拔                                                                                                                 |
| 正常输出锁序为 `console → UART`，UART 中断仅取设备锁         | 避免锁环；panic 时先关中断再切入免等待锁的轮询路径（见 [debug.md](debug.md)）                                                                        |
| 切换上下文时要求关中断且不持自旋锁                           | 调度与唤醒契约见 [scheduling.md](scheduling.md)                                                                                                      |

## 当前边界与扩展前提

- **最小用户态能力**：当前仅支持加载嵌入内核的静态 ELF64 镜像与基础系统调用（`read`/`write`/`exit`/`shutdown`）；文件系统、动态加载、`fork`/`exec`/`wait` 与文件描述符表尚未实现。
- **单 hart 外部中断**：PLIC 当前固定使用 QEMU `virt` hart 0 的 S-mode context 1；支持多 hart 外部中断需建立 `hartid → PLIC context` 映射。
- **无跨 hart 调度与 TLB shootdown**：跨 hart 迁移、远程唤醒及动态撤销共享映射均需引入 IPI 与并发同步协议。
- **无动态资源回收**：已退出或异常终止的用户线程进入 `ThreadState::Exited` 停驻，尚无异步 reaper 回收进程/线程/页表对象，亦无堆页回收与设备卸载路径；浮点上下文未在线程切换时保存恢复。
