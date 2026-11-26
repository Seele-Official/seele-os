# 调度与执行上下文

本文说明内核线程、上下文切换、trap 分发、睡眠/唤醒与启动交接契约。启动全貌见 [architecture.md](architecture.md)，栈窗口与页表见 [memory.md](memory.md)，UART 等待细节见 [platform.md](platform.md)。

## 调度模型概览

- 调度单位是 `proc::thread`；`proc::process` 承载进程页表与线程计数。全系统内核线程共享静态存储中的系统进程 `proc::kernel_proc()`（根页表为 `vm::k::pagetable()`）；用户进程（如 U-mode `shell`）持有 `vm::k::create_process_pagetable()` 派生的独立页表。
- 每个 hart 维护独立的 `idle`、`current`、`ready` 与 `sleeping`；`idle` 不加入 `ready`，仅在无可运行线程时兜底。
- boot hart 在 `char_device::active() != nullptr` 时加载 `#embed` 的 ELF64 `shell` 并创建独立用户进程及其 U-mode 线程（测试构建创建 `ktest` 运行线程）；secondary hart 在系统进程内创建各自的 `idle`（及测试工作线程）。
- `kerneltrap()` 与 `usertrap()` 在定时器和外部中断结束后均调用 `sched::schedule()`；线程可主动调用 `sched::sleep()`、`sched::yield()` 或在退出/异常时调用 `sched::exit_current()`。

## 进程、线程与布局契约

实现见 [process.h](../src/kernel/proc/process.h) 与 [process.cc](../src/kernel/proc/process.cc)。

`process` 持有完整 `satp` 值、Sv39 根表物理地址 `pagetable`、原子引用计数 `ref_count`、活跃线程计数 `thread_count`，以及由 `state_lock` 保护的 `ProcessState`（`Alive` / `Exiting` / `Exited`）、`exit_code`、`fault_cause` 与一次性退出同步原语 `Latch exited`。用户进程通过轻量 RAII 引用计数句柄 `proc::ProcessHandle` 管理外部生命周期（支持拷贝/移动、`wait()` 阻塞等待与 `try_status()` 非阻塞查询）；进程内每个活跃线程额外持有一份内部引用。`thread` 仅作为无退出态/退出码的轻量调度单位，指向所属 `process`，保存 `kernel_context`、`kstack { base, size }`（位于全局共享根项 508 窗口，同时供 [stacktrace.cc](../src/kernel/stacktrace.cc) 限定回溯范围）、内核线程入口 `entry`、原子调度状态 `state`（`ThreadState`）、所属 hart 指针 `hart`（`cpu*`）与链表指针。`trap_frame` 不再内嵌在 `thread` 内，而是统一保存在当前线程的内核栈上。

`proc::make_thread(owner, entry)` 仅用于 `proc::kernel_proc()`，必须在当前 hart 启用可访问共享堆与内核栈窗口的页表后调用：经 `vm::k::alloc_kernel_stack()` 分配全局唯一带 guard 页栈槽并分配线程对象，经内部 `kernel_thread_trampoline` 先执行 `proc::reap_dying()` 再进入 `entry`（若普通内核线程从 `entry` 返回则自动退出回收），初始化 `sp = 槽顶`、`s0..s11 = 0`、`state = Created`、`hart = nullptr`。用户线程通过 `proc::make_user_thread(owner, entry_pc, user_sp)` 创建（当前约束单线程用户进程，`owner` 须处于 `Alive` 且 `thread_count == 0`）：为线程增加进程引用与活跃线程计数，在内核栈顶部预置 272 字节初始 `trap_frame`（`sepc = entry_pc`、`sp = user_sp`、`SPP = 0`、`SIE = 0`、`SPIE = 1`），首次被调度时经内部 `user_thread_trampoline` 调用 `proc::reap_dying()` 后经 `trap::enter_user()` 切入 U-mode。二者均**不加入 `ready` 队列**（须经 `sched::enqueue()` 绑定至当前 hart 并入队）。栈窗口与 guard 页布局见 [memory.md](memory.md) 与 [vm.h](../src/kernel/mm/vm.h)。

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

`tp` 存当前 hart 的 `cpuid`，内核不使用 `gp`（链接启用 `--no-relax-gp`），二者均不随线程切换；`switch_thread()` 不修改中断状态，也不保存/恢复浮点寄存器。

## Trap 入口与分发

入口与接口见 [tvec.S](../src/kernel/tvec.S)、[trap.h](../src/kernel/trap.h)、[trap.cc](../src/kernel/trap.cc) 与 [hart.h](../src/kernel/hart.h)。每个 hart 在 [kmain.cc](../src/kernel/kmain.cc) 初始化 `cpu::trap_scratch.kernel_tp = cpuid()`，`idle()` 先清零 `sscratch` 再将页对齐的 `tvec` 装入 `stvec`（direct 模式）。

内核与用户路径共享统一的栈上 272 字节（16 字节对齐）`trap::trap_frame`（含 `x1..x31`、`sepc`、`sstatus` 与对齐填充），并以 `sscratch` 区分来源：

- **内核路径（`sscratch == 0`）**：在当前线程内核栈分配 272 字节 `trap_frame`，保存 `x1..x31`（其中 `frame->sp` 为陷入前的原 `sp`）、`sepc` 与 `sstatus`，保持 `s0` 不变以支持帧指针回溯；调用 `kerneltrap(trap_frame*)` 返回后由汇编从 `trap_frame` 恢复 `sepc`、`sstatus` 与 `x1, x3, x5..x31`（不恢复 `sp` 与 `tp`），回收栈帧并 `sret`。
- **用户路径（`sscratch != 0`）**：`sscratch` 指向本 hart 的 24 字节 `trap::trap_scratch`（`kernel_sp`、`kernel_tp`、`user_sp_tmp`）。陷入时暂存用户 `sp` 至 `user_sp_tmp`，切至 `kernel_sp` 并在内核栈分配 272 字节 `trap_frame`，保存全部用户 `x1..x31`、`sepc` 与 `sstatus` 同时清零 `sscratch`（此后嵌套 trap 走内核路径）；再恢复内核 `tp`（`kernel_tp`）、清零 `s0` 截断跨特权级回溯，调用 `usertrap(trap_frame*)`。`usertrap()` 处理 `TIME_INT`（重装 `stimecmp` 并调度）、`EXT_INT`（服务外部中断并调度）、`U_ECALL`（推进 `frame->sepc += 4`，开中断执行 [syscall.cc](../src/kernel/proc/syscall.cc) 分发后关中断写回 `a0`），以及未处理用户异常（打印错误日志并调用 `proc::exit_current_process(-1, cause)` 以进程异常终止语义退出并回收资源，而不导致内核 panic）。返回时在关中断状态下交回当前 hart 的 `trap_scratch*`，落入 `tvecret` 写回 `kernel_sp`、恢复 `sepc`/`sstatus`、重装 `sscratch`、恢复全部用户寄存器并 `sret`。首次进入用户态的入口为 `trap::enter_user(frame, scratch)`。

### 系统调用（U-mode `ecall`）

共享定义见 [src/uapi/syscall.h](../src/uapi/syscall.h)，分发实现见 [src/kernel/proc/syscall.cc](../src/kernel/proc/syscall.cc)。采用标准 RISC-V ABI（`a7` 为调用号，`a0..a5` 为参数，`a0` 返回非负结果或负错误码）：

| 调用号 | 名称         | 语义                                                                                                                                                                         |
| ------ | ------------ | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `63`   | `READ`       | 经 `char_device::active()->read()` 从 `STDIN_FD`（0）阻塞读取至多 64 字节（钳位在首用户页内并在阻塞前校验写权限，未挂接设备返回 `-ENODEV`）                                  |
| `64`   | `WRITE`      | 经 `vm::u::copy_from_user()` 分块（128 字节，单次上限 4 KiB）调用可调度的 `char_device::active()->write()` 向 `STDOUT_FD`/`STDERR_FD`（1/2）输出（未挂接设备返回 `-ENODEV`） |
| `94`   | `EXIT_GROUP` | 调用 `proc::exit_current_process(status)` 记录进程退出码并退出当前用户进程，切出后即时回收线程、内核栈与用户地址空间（对齐 Linux RISC-V `exit_group`）                       |
| `1000` | `SHUTDOWN`   | 若已挂接关机设备则调用 `power::poweroff(code)` 关机，否则返回 `-ENODEV`                                                                                                      |

### `kerneltrap()` 分发

| cause          | 行为                                                       |
| -------------- | ---------------------------------------------------------- |
| `TIME_INT`     | 设置 `stimecmp = time + 1000000`，随后 `sched::schedule()` |
| `EXT_INT`      | 调用已安装控制器的 `handle()`，随后 `sched::schedule()`    |
| 其他异常或中断 | 打印 `cause`、`sepc`、`stval`、`sstatus` 与调用栈后 panic  |

定时器间隔固定为 1,000,000 个 time tick（未按 `timebase_frequency` 换算；10 MHz 下为 100 ms）。`sepc` 与 `sstatus` 由 `tvec.S` 统一保存并恢复于当前线程内核栈上的 `trap_frame`，保证跨线程切换后各线程 `sret` 恢复自身现场。常规时钟与外部中断不输出日志。

## 调度与等待契约

实现见 [schedule.h](../src/kernel/sched/schedule.h) / [schedule.cc](../src/kernel/sched/schedule.cc)，采用每 hart FIFO 轮转并显式维护 `proc::ThreadState`（`Created` / `Ready` / `Running` / `Blocking` / `Blocked`）与所属 `hart`（`cpu*`）：

- `enqueue(thread)`：将新建线程（`Created`、`hart == nullptr`）绑定到当前 hart 并加入 `ready` 尾部（转为 `Ready`）；`idle` 不通过该入口入队。
- `schedule()` / `yield()`：从 `ready` 头部取下一线程；`ready` 为空时继续当前线程（故 `shell` 可运行且无其他就绪线程时不会轮转到 `idle`）。切换时将当前线程转为 `Ready`（非 `idle` 则放回 `ready` 尾部），目标线程转为 `Running`，切回后立即调用 `proc::reap_dying()`；若 `idle` 切回时因回收唤醒了同 hart 等待者，则在 `reschedule()` 内直接再切入该等待者而不必回落到 `wfi()`。
- `prepare_sleep()` + `sleep()`：两阶段阻塞握手。调用方先关本地中断，在持有资源锁并确认需等待后调用 `prepare_sleep()` 将当前线程标为 `Blocking` 再登记等待者，随后释放资源锁（保持 `SIE = 0`）并调用 `sleep()`。若期间并发的 `wake()` 已将 `Blocking` 改回 `Running`，`sleep()` 直接返回不切出；否则转为 `Blocked` 移入 `sleeping`，运行 `ready` 头部（无则回落 `idle`），切回后调用 `proc::reap_dying()`。`idle` 不可睡眠。
- `wake(thread)`：直接读取 `thread->hart` 并加 `target_cpu->lock`（支持跨 hart 及中断上下文调用）：
    - `Blocking -> Running`：提前到达的唤醒，供 `sleep()` 消费而不挂起；
    - `Blocked -> Ready`：O(1) 移出目标 hart 的 `sleeping` 队列并加入其 `ready` 尾部；
    - 其他状态、`nullptr` 或未绑定线程为无操作。当前不发跨 hart IPI，被唤醒线程若所在 hart 处于 `idle` 将在下次时钟或外部中断时运行。

- `exit_current()`：将当前非 `idle` 线程挂入本 hart 的 `cpu::dying_thread`（不再放回 `ready`/`sleeping`），随后切向下一就绪线程（空则回落 `idle`）。目标线程在 `switch_thread()` 之后（或新线程 trampoline 入口）立即调用 `proc::reap_dying()` 归还内核栈槽并释放线程对象；若为用户进程的最后一个活跃线程，同时释放其用户页表与物理页、将进程置为 `ProcessState::Exited`、唤醒所有 `ProcessHandle::wait()` 等待者，并在 `ref_count` 归零时立即销毁 `process` 对象（无外部句柄的 `shell` 等 detached 进程自动全量回收）。

队列与 `current` 由各自 `cpu->lock` 保护，更新 `current` 并释放锁后再执行切换。**所有触发切换的原语均要求调用方关中断（`SIE = 0`）且不持有任何自旋锁**。

### 等待队列、互斥锁、信号量与 Latch

- [wait_queue.h](../src/kernel/concurrency/wait_queue.h) / [wait_queue.cc](../src/kernel/concurrency/wait_queue.cc) 提供外部加锁的 FIFO `WaitQueue`：等待者节点 `WaitQueue::waiter` 分配在阻塞线程自身内核栈上（因根项 508 内核栈窗口在所有进程页表间全局共享，跨 hart、跨进程与中断上下文均可直接摘链），零堆分配且 `WaitQueue` 平凡可析构（可用于静态存储）。`prepare_wait()` 负责调用 `sched::prepare_sleep()` 并入队；唤醒方须在持有外部锁期间通过 `pop_one()` 或 `take_all()` 摘除节点，释放锁后再调用 `sched::wake()` / `wake_all()`。
- [latch.h](../src/kernel/concurrency/latch.h) / [latch.cc](../src/kernel/concurrency/latch.cc) 基于 `Spinlock` 与 `WaitQueue` 实现一次性向下计数同步原语 `Latch`（对齐 C++20 `std::latch` 语义，提供 `count_down()` / `try_wait()` / `wait()` / `arrive_and_wait()`），平凡可析构；计数归零后永久保持打开并广播唤醒全部等待者，`count_down()` 仅使用自旋锁，可安全在中断或切出后回收上下文调用（用于 `process::exited` 进程退出等待）。
- [mutex.h](../src/kernel/concurrency/mutex.h) / [mutex.cc](../src/kernel/concurrency/mutex.cc) 基于 `Spinlock` 与 `WaitQueue` 实现不可重入的可睡眠互斥锁 `Mutex`（`lock()` / `try_lock()` / `unlock()`，可直接配合 `LockGuard` 使用），平凡可析构；显式追踪持有线程 `proc::thread*`（递归加锁或非持有者解锁触发 panic），并采用直接所有权交接（FIFO handoff）：`unlock()` 唤醒队首等待者时直接将所有权移交该线程，避免插队与惊群。
- [semaphore.h](../src/kernel/concurrency/semaphore.h) / [semaphore.cc](../src/kernel/concurrency/semaphore.cc) 基于 `Spinlock` 与 `WaitQueue` 实现计数信号量 `Semaphore`（`acquire()` / `try_acquire()` / `release()`），平凡可析构，采用直接名额交接（handoff）：`release()` 唤醒队首等待者时直接移交名额而不递增计数。

## 启动交接

在 [kmain.cc](../src/kernel/kmain.cc) 中，boot hart 建立并启用唯一内核根表后调用 `proc::init()` 初始化系统进程，创建 `idle`、探测驱动并按需入队 `shell`（或 `ktest` 运行线程），再 release 发布 `g_started`；secondary hart acquire 读取 `g_started` 后直接启用同一内核根表并在 `proc::kernel_proc()` 中创建各自 `idle`。

每个 hart 将 `idle` 绑定至本 hart 并标为 `Running`、置 `current = idle`，用一次性的零初始化 `proc::thread old{}` 承接 boot 上下文并调用 `switch_thread(&old, idle)`（`old.proc == nullptr` 触发同值 `satp` 写入，boot 上下文此后不再恢复）。`idle()` 运行于各自独立分配的共享内核栈槽上，先将本 hart 的 boot 栈逐页归还物理分配器，再配置定时器、`stvec`、`sie.SEIE | sie.STIE` 并打开 `sstatus.SIE`，随后循环 `wfi()`。用户态 `shell` 首次运行时经 trampoline 由 `trap::enter_user()` 切入 U-mode 并输出提示符（见 [shell.cc](../src/kernel/proc/shell.cc) 与 [src/user/shell/main.cc](../src/user/shell/main.cc)）。

## 当前限制

- 当前仅支持单线程用户进程与内核侧 `ProcessHandle::wait()`；用户态 `fork`/`exec`/`wait` 系统调用、多线程用户进程与兄弟线程取消尚未实现。
- 无时间片记账、优先级、负载均衡、线程迁移或跨 hart 唤醒 IPI（`wake()` 已支持跨 hart 入队至目标 hart 的 `ready`，但不会主动打断远端 `idle` 的 `wfi()`）。
- `start()` 将 `mstatus.FS` 置为 Initial，但上下文切换不保存/恢复浮点寄存器。
- 未处理异常直接 panic 停机（诊断见 [debug.md](debug.md)）；修改 `trap_frame` / `trap_scratch` 须同步更新 `trap.h`、`tvec.S` 与布局断言。
