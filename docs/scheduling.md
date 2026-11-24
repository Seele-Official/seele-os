# 调度与执行上下文

本文说明内核线程、上下文切换、trap 分发、睡眠/唤醒与启动交接契约。启动全貌见 [architecture.md](architecture.md)，栈窗口与页表见 [memory.md](memory.md)，UART 等待细节见 [platform.md](platform.md)。

## 调度模型概览

- 调度单位是 `proc::thread`；`proc::process` 承载共享页表与栈槽位计数。
- 每个 hart 维护独立的 `idle`、`current`、`ready` 与 `sleeping`；`idle` 不加入 `ready`，仅在无可运行线程时兜底。
- boot hart 在 `console::can_read()` 为真时于同一进程内创建 `shell` 线程；secondary hart 仅运行 `idle`。
- `kerneltrap()` 在定时器和外部中断结束后调用 `sched::schedule()`；线程可主动调用 `sched::sleep()`，另有 `sched::yield()` 入口（当前无调用方）。

## 进程、线程与布局契约

实现见 [process.h](../src/kernel/proc/process.h) 与 [process.cc](../src/kernel/proc/process.cc)。

`process` 持有完整 `satp` 值、Sv39 根表物理地址 `pagetable` 及原子栈槽计数 `thread_count`；`thread` 指向所属 `process`，保存 `kernel_context`、`kstack { base, size }`（同时供 [stacktrace.cc](../src/kernel/stacktrace.cc) 限定回溯范围）、预留用户 `trap_frame` 与链表指针。

`proc::make_thread(owner, entry)` 必须在启用可访问堆窗口的页表后调用：原子预留槽位、映射私有栈页并分配线程对象，初始化 `ra = entry`、`sp = 槽顶`、`s0..s11 = 0`。它仅创建线程，**不加入 `ready` 队列**；当前所有线程均在 `kmain()` 中创建，线程入口不返回。栈窗口与 guard 页布局见 [memory.md](memory.md) 与 [vm.h](../src/kernel/mm/vm.h)。

### 汇编布局契约

[process.h](../src/kernel/proc/process.h) 通过 `static_assert` 锁定以下偏移供 [switch.S](../src/kernel/sched/switch.S) 使用（修改相关结构须同步更新断言与汇编）：

| 成员                              | 字节偏移/大小                             |
| --------------------------------- | ----------------------------------------- |
| `process::satp`                   | 偏移 0                                    |
| `thread::proc`                    | 首成员，偏移 0                            |
| `thread::context`                 | 偏移 8                                    |
| `kernel_context::{ra, sp, s[12]}` | 相对 context 偏移 0、8、16，总长 112 字节 |

## 上下文切换

`sched::switch_thread(from, to)` 执行：

1. 将 `ra`、`sp`、`s0..s11` 保存至 `from->context`。
2. 若 `from->proc != to->proc`，将 `to->proc->satp` 写入 `satp`，前后各执行一次 `sfence.vma`；同进程切换跳过 `satp` 写入与 TLB 刷新。
3. 恢复 `to->context` 寄存器并 `ret` 至其 `ra`（已有线程从调用点继续，新线程进入 `entry`）。

`gp` 为内核全局指针、`tp` 存当前 hart 的 `cpuid`，二者不随线程切换；`switch_thread()` 不修改中断状态，也不保存/恢复浮点寄存器。

## Trap 入口与分发

入口与接口见 [tvec.S](../src/kernel/tvec.S)、[trap.h](../src/kernel/trap.h) 与 [trap.cc](../src/kernel/trap.cc)。`idle()` 将页对齐的 `tvec` 装入 `stvec`（direct 模式），以 `sscratch` 区分来源：

- **内核路径（`sscratch == 0`）**：在当前线程栈开辟 256 字节帧保存 caller-saved 寄存器及 `gp`，调用 `kerneltrap()` 后恢复并 `sret`。
- **预留用户路径（`sscratch != 0`）**：`sscratch` 指向 `trap_frame`，保存 `x1..x31`、`sepc` 与 `sstatus` 后切入内核栈。当前无构造用户帧或进入用户态的路径，`usertrap()` 仅记录日志与转发外部中断。

### `kerneltrap()` 分发

| cause          | 行为                                                       |
| -------------- | ---------------------------------------------------------- |
| `TIME_INT`     | 设置 `stimecmp = time + 1000000`，随后 `sched::schedule()` |
| `EXT_INT`      | 调用已安装控制器的 `handle()`，随后 `sched::schedule()`    |
| 其他异常或中断 | 打印 `cause`、`sepc`、`stval`、`sstatus` 与调用栈后 panic  |

定时器间隔固定为 1,000,000 个 time tick（未按 `timebase_frequency` 换算；10 MHz 下为 100 ms）。`kerneltrap()` 将 `sepc` 与 `sstatus` 保存在本线程栈的局部变量中并在返回前写回，保证跨线程切换后各线程 `sret` 恢复自身现场。常规时钟与外部中断不输出日志。

## 调度与等待契约

实现见 [schedule.h](../src/kernel/sched/schedule.h) / [schedule.cc](../src/kernel/sched/schedule.cc)，采用 hart 本地 FIFO 轮转：

- `schedule()` / `yield()`：从 `ready` 头部取下一线程；`ready` 为空时继续当前线程（故 `shell` 可运行且无其他就绪线程时不会轮转到 `idle`）。切换时将非 `idle` 的当前线程放回 `ready` 尾部。
- `sleep()`：将当前线程移入 `sleeping`，运行 `ready` 头部，无则回落 `idle`；`idle` 不可睡眠。
- `wake(thread)`：仅扫描本 hart 的 `sleeping` 队列，命中则移至 `ready` 尾部，未命中无操作（**不缓存提前到达的唤醒**）。

队列与 `current` 由 `cpu->lock` 保护，更新 `current` 并释放锁后再执行切换。**所有触发切换的原语均要求调用方关中断（`SIE = 0`）且不持有任何自旋锁**。`wake()` 可从同 hart 的中断处理器调用，仅更新队列不直接切换。

为消除“条件检查 → 登记等待 → `sleep()`”之间的丢失唤醒窗口，等待者须在整个窗口保持 `SIE = 0`，并在进入 `sleep()` 前释放资源锁（具体 UART 实现见 [platform.md](platform.md)）。

## 启动交接

在 [kmain.cc](../src/kernel/kmain.cc) 中，boot hart 初始化页表、idle 与驱动后按需创建 shell 并加入 `ready`，再 release 发布 `g_started`；secondary hart acquire 读取 `g_started` 后建立各自页表与 idle。

每个 hart 将 `current` 置为 `idle`，用一次性的零初始化 `proc::thread old{}` 承接 boot 上下文并调用 `switch_thread(&old, idle)`（`old.proc == nullptr` 触发同值 `satp` 写入，boot 上下文此后不再恢复）。`idle()` 运行于槽 0 栈上，先将本 hart 的 boot 栈逐页归还物理分配器，再配置定时器、`stvec`、`sie.SEIE | sie.STIE` 并打开 `sstatus.SIE`，随后循环 `wfi()`。`shell` 首次运行时自行打开 `SIE`、执行 `console::flush()` 并输出提示符（见 [shell.cc](../src/kernel/proc/shell.cc)）。

## 当前限制

- 初始线程集在启动期固定，全部运行于内核态；无运行时并发创建、线程退出、wait 或槽位/资源回收。
- 无时间片记账、优先级、负载均衡、线程迁移或跨 hart 唤醒。
- `start()` 将 `mstatus.FS` 置为 Initial，但上下文切换不保存/恢复浮点寄存器。
- 未处理异常直接 panic 停机（诊断见 [debug.md](debug.md)）；修改 trap frame 须同步更新 `trap.h`、`tvec.S` 与布局断言。
