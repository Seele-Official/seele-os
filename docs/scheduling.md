# 调度与执行上下文

本文说明当前内核线程如何运行、阻塞和恢复，以及 C++、汇编和驱动之间的契约。启动全貌见 [architecture.md](architecture.md)，栈窗口与页表见 [memory.md](memory.md)，UART 等待机制见 [platform.md](platform.md#输入与调度器契约)。

## 当前行为

- 调度单位是 `proc::thread`；`proc::process` 保存共享地址空间，不直接参与选取。
- 每个 hart 有自己的 idle、current、ready 和 sleeping；idle 不进入 ready。
- boot hart 仅在 `console::attached()` 为真时创建 shell，它与 idle 属于同一个进程、使用不同栈槽位；secondary 当前只有 idle。
- 内核 trap 路径的定时器和外部中断在处理后调用 `shed::schedule()`；线程也可以主动 `shed::sleep()`。`shed::yield()` 已提供，但当前没有调用者。
- 所有运行中的线程都在内核态；没有线程退出、资源回收或跨 hart 调度。

导航：[进程、线程与栈](#进程线程与栈) · [上下文切换](#上下文切换) · [Trap](#trap-入口与返回) · [调度与等待](#调度与等待契约) · [启动交接](#启动交接) · [限制](#当前限制) · [开发注意事项](#开发注意事项)

## 进程、线程与栈

实现见 [process.h](../src/kernel/proc/process.h) 和 [process.cc](../src/kernel/proc/process.cc)。

`process` 保存完整的 `satp` CSR 值、Sv39 根表物理地址 `pagetable` 和栈槽位计数 `thread_count`。`thread` 指向所属进程，保存 `kernel_context`、`kstack { base, size }`、预留的用户 `trap_frame` 及链表指针。一个进程的线程共享页表，但各有独立内核栈。

`proc::make_thread(owner, entry)` 原子递增 `thread_count` 预留槽位，检查 `slot < KSTACK_MAX_THREADS`，映射栈后分配线程对象；初始 `ra = entry`、`sp = 槽位栈顶`，`s0..s11` 清零。它只创建线程，**不会将其加入 ready 或立即运行**。当前所有创建都发生在 `kmain()`，没有运行期创建或销毁路径。

每个进程拥有 128 KiB 的私有虚拟栈窗口（Sv39 根表项 508），各槽位由 `KSTACK_SIZE` 字节映射和下方一页未映射 guard 组成。容量推导与映射约束以 [内存文档](memory.md)和 [vm.h](../src/kernel/mm/vm.h) 为准。`thread::kstack` 也供 [stacktrace](../src/kernel/stacktrace.cc) 限定回溯范围。guard 触发 page fault 不代表可恢复：当前异常只记录日志，不能修复栈溢出。

### 汇编布局契约

[process.h](../src/kernel/proc/process.h) 的断言固定了以下布局，供 [switch.S](../src/kernel/shed/switch.S) 使用：

| 对象/成员                         | 字节偏移或大小                            |
| --------------------------------- | ----------------------------------------- |
| `process::satp`                   | 偏移 0                                    |
| `thread::proc`                    | 首成员，偏移 0                            |
| `thread::context`                 | 偏移 8                                    |
| `kernel_context::{ra, sp, s[12]}` | 相对 context 偏移 0、8、16，总长 112 字节 |

修改这些成员时必须同步检查汇编偏移和 `static_assert`，不要把文档中的布局视为完整结构定义。

## 上下文切换

`shed::switch_thread(from, to)` 执行以下操作：

1. 将 `ra`、`sp`、`s0..s11` 保存到 `from->context`。
2. 若进程指针不同，将 `to->proc->satp` 写入 `satp`，写入前后各执行一次 `sfence.vma`；同进程切换不改 `satp`，也不刷新 TLB。
3. 恢复 `to->context` 的寄存器，`ret` 到其 `ra`。

这是可恢复的函数调用：`from` 再次被调度时从该调用之后继续。首次运行的新线程则进入 `make_thread()` 指定的入口。当前入口不应返回，没有线程返回后的退出处理。

切换只保存上述 C ABI 所需状态；trap 路径另存被中断代码的 caller-saved 寄存器。`gp` 是内核状态，`tp` 保存 hart 的 cpuid，不随线程切换；`switch_thread()` 本身不保存或恢复中断状态，也不切换浮点寄存器。

## Trap 入口与返回

入口与帧布局分别见 [tvec.S](../src/kernel/tvec.S)、[trap.h](../src/kernel/trap.h)，处理逻辑见 [trap.cc](../src/kernel/trap.cc)。`idle()` 将页对齐的 `tvec` 写入 `stvec`（direct 模式）。`sscratch` 是软件约定的路径标记：

- **当前内核路径**：`sscratch == 0`。在当前线程栈上预留 256 字节，保存 caller-saved 寄存器（以及 `gp`），调用 `kerneltrap()`，随后恢复并 `sret`。
- **尚未启用的用户路径**：`sscratch` 指向 `trap_frame`。入口保存 x1..x31、`sepc` 和 `sstatus`，清空 `sscratch`，从 frame 取得内核栈、hart 标识和 `next_call`，返回后恢复 frame 并 `sret`。当前没有准备用户 frame 或进入用户态的代码；`usertrap()` 仅记录信息、服务外部中断并返回原 frame，不能视为已实现用户态调度或系统调用。

### 当前 `kerneltrap()` 分发

| cause          | 行为                                                       |
| -------------- | ---------------------------------------------------------- |
| `TIME_INT`     | 设置 `stimecmp = time + 1000000`，然后 `shed::schedule()`  |
| `EXT_INT`      | 若已安装控制器则调用其 `handle()`，随后 `shed::schedule()` |
| 其他异常或中断 | 只记录日志，不修复、不推进 `sepc`，也不调度                |

定时器间隔是固定的 1,000,000 个 time tick，**并未按发现的 `timebase_frequency` 换算**；若 timebase 为 10 MHz，则名义间隔为 100 ms。外部中断后的调度让刚被唤醒的线程有机会立即运行，而不必等待下次定时器；这不是延迟保证。定时器和外部中断不逐次记录日志，避免常态日志及 UART 发送中断反馈。

`kerneltrap()` 将 `sepc` 和 `sstatus` 保存在本线程栈上的局部变量中，返回前写回。它们是 hart 级 CSR：本线程被切走期间，其他线程的 trap 会改变它们。各自写回才能让 `sret` 恢复对应线程的返回地址、模式和中断状态。被挂起的 trap 栈帧一直留在原线程栈上。

## 调度与等待契约

实现与接口见 [schedule.cc](../src/kernel/shed/schedule.cc)、[schedule.h](../src/kernel/shed/schedule.h)。这是 hart 本地的简单轮转策略：

- `schedule()` / `yield()`：从 ready 头部取线程；为空则继续当前线程。选到下一线程时，将非 idle 的当前线程放回 ready 尾部。
- `sleep()`：将当前线程加入 sleeping，运行 ready 头部；ready 为空则回落到 idle。idle 自身不能睡眠。
- `wake(thread)`：仅扫描调用 hart 的 sleeping，找到后移到 ready 尾部；找不到则无操作，**不保存提前到达的唤醒请求**。

链表和 `current` 的变更由 `cpu->lock` 保护；先更新 `current`，释放锁后才切换。**会切换的操作要求本 hart 中断关闭且不持自旋锁**。`wake()` 可以从中断 handler 调用，但必须位于目标线程睡眠的同一 hart，且它只改变可运行性，不直接切换。

等待者负责消除“检查条件 → 登记等待 → 进入 sleeping”之间的丢失唤醒窗口。当前 UART 读者在整个窗口内保持 `SIE=0`，只在检查队列和登记 reader 时持设备锁，进入 `sleep()` 前释放设备锁。接收 handler 在释放设备锁后 `wake()`，切换交给外部中断处理末尾的 `schedule()`。完整输入限制与轮询回退见 [平台文档](platform.md#输入与调度器契约)。这依赖接收中断与读者在同一 hart，不能直接推广为跨 hart 等待协议。

idle 是无可运行线程时的回落目标，不是轮转候选。因此 boot hart **不会定时在 idle 与 shell 之间交替**：shell 可运行且 ready 为空时继续运行；它等待输入睡眠后才回到 idle。

## 启动交接

[kmain.cc](../src/kernel/kmain.cc) 为每个 hart 创建并启用进程页表，然后创建 idle。boot hart 在驱动 probe 后按 console 是否已挂接决定是否创建 shell 并加入 ready；secondary 等待 `started` 的 release/acquire 发布后建立自己的页表与 idle，不重复设备发现。

每个 hart 都先设 `current = idle`，再用一次性的零初始化 `proc::thread old{}` 接收 boot 上下文，调用 `switch_thread(&old, idle)`。`old.proc == nullptr` 使首次切换执行 `satp` 写入；此时目标页表已经启用，重写同值。boot 上下文不加入调度，禁止恢复。

`idle()` 已运行在自己的 0 号栈槽位上，先将本 hart 的 boot 栈页归还物理分配器，再设置定时器、trap 入口和 `sie.SEIE | sie.STIE`，打开 `sstatus.SIE` 后循环 `wfi()`。shell 初次进入时自行打开 `SIE`、`console::flush()`，再输出提示符，见 [shell.cc](../src/kernel/proc/shell.cc)。

## 当前限制

- 当前线程集在启动期固定，只有内核线程；没有 `exit`、wait、槽位回收或资源释放语义。
- 定时器提供周期调度点，但没有独立的时间片记账、优先级、公平性或实时性保证；没有负载均衡、线程迁移或跨 hart 唤醒。
- `start()` 将 `mstatus.FS` 置为 Initial，但没有 F/D 保存恢复策略，浮点状态不能保证跨线程保持。
- 异常没有恢复路径；panic 后停机，不恢复当前线程调度。诊断行为见 [debug.md](debug.md)。

## 开发注意事项

- 创建进程/线程前，调用者必须已启用可访问堆窗口的页表；线程栈映射写入所属进程的私有子树。槽位计数原子化只保证编号不重复，不等于整个线程创建路径支持并发。
- 离开 boot 上下文后，`mycpu()->current` 必须指向当前线程；线程从自己的内核栈恢复，不得在持锁区中调用会切换的接口。
- 增加跨 hart 调度必须同时处理 hart 级状态、页表同步和设备等待归属，不能只共享 ready 链表。
- 修改 trap frame 时同步修改 `trap.h`、`tvec.S` 及其布局断言；使用 F/D 前先定义保存恢复策略。
