# 平台层：FDT、设备、驱动与外设

本文说明固件描述到设备资源的转换，以及驱动、MMIO、外部中断和 console/UART 之间的契约。启动全貌见 [architecture.md](architecture.md)，页表映射见 [memory.md](memory.md)，调度等待见 [scheduling.md](scheduling.md)，日志与 panic 诊断见 [debug.md](debug.md)。

## 平台初始化与生命周期

在 [kmain.cc](../src/kernel/kmain.cc) 启动主线中，boot hart 依次完成：

1. `platform::init(dtb, hartid)`：在 [platform.cc](../src/kernel/platform/platform.cc) 静态存储中构造 `fdt::Tree`，尽力挂接 early console 后断言树有效；接着执行 `device::enumerate()` 并填充只读 `platform::Info`。
2. 物理池、内核页表（含设备 MMIO 映射）与首个进程页表就绪并启用分页，创建 idle 进程/线程。
3. `driver::probe_all()` 按 `Core → Device → Late` 探测并挂接驱动（当前为 PLIC、`sifive_test` 与 Ns16550）。
4. 默认构建在 `char_device::active() != nullptr` 时创建 shell（测试构建创建 `ktest` 运行线程），随后 release 发布 `g_started`。

secondary hart acquire 等待 `g_started`，不重复平台发现或驱动 probe。

**生命周期契约**：设备节点偏移依赖静态 `fdt::Tree`，设备名与属性 `string_view` 借用原始 DTB，二者均存活至内核结束。`platform::init()` 首先将 DTB 自身加入 `MemoryMap::reserved()`，防止其所在物理页被物理分配器回收。

## FDT 与启动发现

### 只读 FDT 解析器

[fdt.h](../src/kernel/fdt/fdt.h) 与 [fdt.cc](../src/kernel/fdt/fdt.cc) 提供无动态分配的只读查询接口（各类型化读取方法签名与边界见头文件）：

- 构造时校验 magic、`totalsize`（上限 16 MiB）、版本 17、`last_comp_version <= 17`、结构块/字符串块范围与根节点起始 token；调用方须提供可读 blob，内核启动断言树有效。
- `find()` 接受绝对路径（分量可省略 unit address，不解析 `/aliases`）；`walk()` 以非递归前序遍历结构块，深度受 `SEELE_FDT_MAX_DEPTH` 限制，并向回调传递当前祖先链 `std::span<const Node>`；`by_phandle()` 优先匹配 `phandle`，其次 `linux,phandle`。
- `enabled()` 仅检查节点自身的 `status`（缺省、`okay`、`ok` 为启用），不继承父节点状态。
- `Node::cells()` / `Node::regs()` 沿祖先链解析 `#address-cells` / `#size-cells`（默认 2/2，支持 1 或 2 cell），输出非零长度 `phys_range`，不做总线 `ranges` 地址转换；`#size-cells = 0` 的节点（如 CPU）使用 `Node::reg_address()`。
- `Tree::reserved_regions()` 遍历 `off_mem_rsvmap` 直到 `(0, 0)` 终止项。

### 启动拓扑发现

[platform.h](../src/kernel/platform/platform.h) / [platform.cc](../src/kernel/platform/platform.cc) 将发现结果保存在内核生命周期只读对象 `platform::info()` 中：

| 字段                        | 来源与处理                                                                                                     |
| --------------------------- | -------------------------------------------------------------------------------------------------------------- |
| `MemoryMap::usable()`       | 收集 `device_type = "memory"` 节点的 `reg` bank，按基址排序并合并重叠/相邻区间                                 |
| `MemoryMap::reserved()`     | 合并 DTB 自身、memreserve 块、`/reserved-memory` 下 enabled 子节点 `reg` 及 `linux,initrd-start/end`           |
| `harts()` / `boot_hartid()` | `/cpus` 下 enabled 的 `cpu` 节点（`Node::reg_address()` 解码 hartid 与 `riscv,isa`）；引导 hart 另存为独立标量 |
| `timebase_frequency()`      | `/cpus/timebase-frequency`，缺失时回退根节点同名属性                                                           |
| `cmdline()`                 | `/chosen/bootargs` 原始字符串                                                                                  |

`usable()` 与 `reserved()` 各自保存在容量为 `SEELE_MAX_REGIONS` 的静态数组中（溢出或地址回绕时告警并丢弃）；hart 数组上限为 `SEELE_MAX_HARTS`（构建参数见 [build.md](build.md)）。

## 设备与驱动模型

### 设备枚举

[device.h](../src/kernel/platform/device.h) / [device.cc](../src/kernel/platform/device.cc) 将固件描述与驱动实例解耦，由静态池持有设备及其强类型资源切片：

- `mmios()`：`phys_range`（物理基址与长度）。
- `irqs()`：`irq_line`（控制器设备 id、source number、trigger；`trigger == 0` 表示未知）。

`Device` 仅保存 FDT 结构块偏移 `m_fdt_offset`（向驱动隐藏 `fdt.h`），设备 id 即 `device::all()` 下标；成功 probe 后才记录借用的 `device::Object*`。

`device::enumerate()` 通过单遍 `tree.walk()` 生成设备并在尾部将 `interrupt-parent` phandle 解析为控制器设备 id：

- 跳过 `reserved-memory` 子树；节点须自身 enabled、具备 `compatible` 且至少包含一个 MMIO 或 IRQ 资源才登记为设备（普通 disabled 节点不阻断子节点遍历）。
- `interrupts` 沿祖先链定位 `interrupt-parent`，支持控制器声明 1 或 2 个 `#interrupt-cells`（不支持 `interrupts-extended`）。若 parent 未成为 `Device`，保留 IRQ 资源并告警，置 `controller = UINT32_MAX`。
- 设备池或资源池满时，回滚该节点已写入的 MMIO/IRQ 资源并丢弃整个节点。

### 驱动匹配、对象与接口查询

[registry.h](../src/kernel/drivers/registry.h)、[registry.cc](../src/kernel/drivers/registry.cc) 与唯一驱动表 [table.cc](../src/kernel/drivers/table.cc) 管理探测：

- 按 `Core → Device → Late` 分级探测，同级内按 priority 降序、注册顺序执行。
- 跨全部描述符为每个 `Unbound` 设备选取最佳匹配（priority 最高、同值取先注册者）并仅 probe 一次；失败标记 `Failed` 且不重试其他驱动。
- `priv_size > 0` 时从 1 KiB、16 字节对齐的静态 bump arena 分配零初始化存储供驱动 placement new（失败不回收，耗尽告警并标记失败）。probe 仅在 boot hart 释放 secondary 前执行，无并发竞争。
- [device_object.h](../src/kernel/device_object.h) 提供无 RTTI 的 `device::Object`、`device::Implements<Interfaces...>` 与 `device::dyn_cast<T>()`：以每个接口对应的模板 `inline constexpr` 字节地址作为类型身份，由 `Implements` 通过 `static_cast` 返回正确偏移的接口指针。
- 核心对 probe 返回的对象非互斥地查询 `trap::ExternalController`、`ConsoleSink`、`CharDevice` 与 `PowerDevice`，分别接入 `trap::set_external_controller()`、`console::attach()`、`char_device::attach()` 和 `power::attach()`。对象与子系统借用指针均无销毁路径，存活至内核结束。

## MMIO 访问契约

[mmio.h](../src/kernel/platform/mmio.h) 的 `mmio::map()` 校验非空范围不越出 `DIRECT_MAP_SIZE`，返回 [memory.md](memory.md) 启动阶段已建立的 direct-map 别名（不新建页表项）。驱动与 early console 均经此接口访问寄存器，不自行转换 `Paddr`。

`Region::read<T>()` / `write<T>()` 执行单次至多 8 字节的 volatile 访问（绝对地址对齐与边界由 Debug 断言校验）；需要硬件访存保序时显式调用 `Region::fence()`（`fence iorw,iorw`）。

## 外部中断：trap 与 PLIC

### 分发协议

[trap.h](../src/kernel/trap.h) / [trap.cc](../src/kernel/trap.cc) 将控制器与设备解耦：控制器实现 `ExternalController::{handle(), enable(irq, priority)}`，设备实现 `ExternalHandler::handle(irq)`；trap 仅借用永久存活的对象指针。

- Core 级 probe 安装控制器后，设备通过 `register_external_handler()` 登记（irq 为 0、重复、表满、无控制器或 `enable()` 失败均返回 `false`；`priority == 0` 表示登记但屏蔽）。控制器与分发表在启动期完成注册，经 `g_started` 发布。
- `kerneltrap()` 收到 `EXT_INT` 后委托控制器 `handle()` 执行 claim → `dispatch_external_handler()` → complete（未命中 handler 同样必须 complete，有限个未登记 source 各告警一次），调度在 trap 返回前统一触发。
- 设备 handler 运行在关中断 trap 上下文中，禁止睡眠或触发递归日志。

### PLIC 驱动

[plic.cc](../src/kernel/drivers/irq/plic.cc) 映射首个 MMIO 资源并校验窗口覆盖 `riscv,ndev` 及目标 context，清空该 context 的 enable 位图并将 threshold 置 0。`handle()` 单次 trap 最多循环处理 `ndev` 次 claim/dispatch/complete，防止持续中断源饿死其他工作。

当前实现固定使用 QEMU `virt` hart 0 的 S-mode context 1（不解析 `interrupts-extended` 或映射其他 context），单控制器按全局 source 编号查表。

## Console 与 UART

### 三路径分层与正交接口

内核串口 I/O 按执行上下文与调度语义拆分为两个正交设备接口，经 `device::Implements` 独立组合（不再存在继承关系）：

- [console_sink.h](../src/kernel/console_sink.h) 定义同步内核输出接口 `ConsoleSink`（负责 **log** 与 **panic** 两条不可调度路径，供 early console 与正式驱动实现）。
- [char_device.h](../src/kernel/char_device.h) / [char_device.cc](../src/kernel/char_device.cc) 定义可调度双向字符流接口 `CharDevice`（负责 **syscall** 读写路径，仅限正式驱动实现）及默认交互式字符设备借用绑定 `char_device::attach()` / `char_device::active()`。
- [console.h](../src/kernel/console.h) / [console.cc](../src/kernel/console.cc) 仅负责同步格式化输出、正常日志串行化、`ConsoleSink` 切换与 panic 紧急通道，不再转发用户态 `read`/`write`。

挂接规则：

- `console::attach_boot()` 在早期无锁挂接纯同步输出后端（此时 `char_device::active()` 仍为 `nullptr`）；`console::attach()` 在 `console` 锁内排空旧 sink 后原子切换输出指针。未挂接 sink 时日志输出丢弃。
- 凡 probe 返回且提供 `ConsoleSink` 或 `CharDevice` 的驱动均按接口分别接入 `console::attach()` 与 `char_device::attach()`（暂不按 `/chosen/stdout-path` 筛选，后挂接者生效）。

### 同步输出与 panic 契约（`ConsoleSink`）

正常日志输出锁序固定为 **`console → 设备锁`**，且永不调用调度器或 `sched::wake()`（不引入 `console → cpu->lock` 锁依赖）。

| 操作                | Ns16550 行为                                                                                                                    |
| ------------------- | ------------------------------------------------------------------------------------------------------------------------------- |
| `write_sync()`      | 持设备锁先轮询排空异步 TX 环（保序），再同步轮询输出日志；若仍有写者睡眠在 `m_tx_waiters`，保持 `IER_TX_EMPTY` 开启交由中断唤醒 |
| `flush()`           | 持设备锁排空异步 TX 环并等待 `LSR.TEMT`（移位寄存器空）；不可从本设备中断处理器调用                                             |
| `write_emergency()` | 无锁轮询直写 THR，不触碰共享队列，供 panic 专用紧急输出通道使用                                                                 |
| `enter_panic()`     | 置设备 panic 闩锁并无锁屏蔽 UART IER，阻止并发路径重开电平触发中断                                                              |

`console::enter_panic()` 发布 console 闩锁并调用 `sink->enter_panic()` 屏蔽设备中断。常规 `print()` / `LOG_*` / `flush()` 见闩锁后直接丢弃；`panic()` 与 `stacktrace::print()` 走专用无锁 `console::print_emergency()` / `println_emergency()` 直写 `write_emergency()`，不触碰 console 或设备锁。

### 可调度读写契约（`CharDevice`）

[ns16550.cc](../src/kernel/drivers/uart/ns16550.cc) 的 `CharDevice::read(span)` 与 `CharDevice::write(span)` 仅允许在可调度线程上下文调用（已有当前线程且非 `idle`、`SIE = 1`、不持自旋锁；`read()` 另要求 **`cpuid == 0`**，因 PLIC 当前将 UART IRQ 路由到 hart 0 且尚无跨 hart 唤醒 IPI），且完全依赖中断驱动（无轮询回退）：

- 由 `m_lock` 统一保护 64 字节 RX 环、128 字节 TX 环及对应的 `WaitQueue`（`m_rx_waiters`、`m_tx_waiters`），另以可睡眠二值 `Semaphore m_tx_writer{1}` 串行化并发的多段大块写（锁序：`m_tx_writer → m_lock`，睡眠前释放 `m_lock`）。
- `read(span)`：关中断取 `m_lock`，有数据则一次尽可能拷出 `1..buffer.size()` 字节；为空且未 panic 时经 `m_rx_waiters.prepare_wait()` 登记后释放锁并 `sched::sleep()`。
- `write(span)`：持 `m_tx_writer`，关中断取 `m_lock` 将字节填入 TX 环并按需开启 `IER_TX_EMPTY`；若尚有剩余字节（TX 环满）则登记 `m_tx_waiters` 后释放锁并 `sched::sleep()`。
- `handle()`：持 `m_lock` 先收后发（各最多处理一个探测 FIFO 深度），有新收字节时摘出 `m_rx_waiters`，TX 环出现空位时摘出 `m_tx_waiters`，仅在 `transmit_count() == 0 && m_tx_waiters.empty()` 时关闭 `IER_TX_EMPTY`；**释放 `m_lock` 后**统一 `wake_all()`，由外部中断末尾的 `schedule()` 切换。

### Early console 与 Ns16550 驱动

两者共享无状态的寄存器定义与参数校验 [ns16550_hw.h](../src/kernel/platform/ns16550_hw.h)：均经 `mmio::map()` 校验 direct-map 边界，非法 `reg-shift` / `reg-io-width` 回退为 `0` / `1`，并校验基址对齐及覆盖寄存器 7 的窗口长度。

- **Early console**（[early_console.cc](../src/kernel/platform/early_console.cc)）：独立于 `drivers/`，仅实现 `ConsoleSink`，无分配、无日志、无锁、无中断（预留同层并列接入 OpenSBI 后端）。优先解析 `/chosen/stdout-path` 绝对路径（去 `:` 选项，校验 enabled 且兼容 `ns16550a`/`ns16550`，命中但 `reg` 无效则不再回退），未命中时 `walk()` 回退至首个匹配的 enabled 节点。挂接失败不影响启动。
- **Ns16550 驱动**（[ns16550.cc](../src/kernel/drivers/uart/ns16550.cc)）：Device 级、priority 0，匹配 `ns16550a`/`ns16550`，实现 `device::Implements<ConsoleSink, CharDevice, trap::ExternalHandler>`。按 `clock-frequency` 与 `SEELE_UART_BAUD` 设置除数（缺失或过小取 1，过大钳至 `0xffff`）、8N1 并清空 FIFO；必须以 priority 1 注册首个 IRQ，缺少可用中断线或注册失败直接 panic（不提供轮询回退）。RX 环满时丢弃新读出字节并递增 `rx_overrun_count()`。`set_interrupt_enable()` 写入前后复查设备 panic 闩锁，防止并发写者重开已屏蔽中断。

## 系统关机：power 与 `sifive_test`

[power.h](../src/kernel/power.h) / [power.cc](../src/kernel/power.cc) 定义硬件无关的 `PowerDevice` 接口与关机入口：

- `power::poweroff(code)`：先调用 `console::flush()` 排空串口缓冲，再关本地中断并写入已挂接 `PowerDevice`（调用方不得持有 `console` 或 UART 锁）；无关机设备或硬件未立即断电时回落 `wfi()`。
- `power::poweroff_emergency(code)`：供测试构建的 `panic()` 使用，不触碰锁或常规输出队列，直接转发给已挂接设备。
- **`sifive_test` 驱动**（[sifive_test.cc](../src/kernel/drivers/power/sifive_test.cc)）：Core 级、priority 0，匹配 QEMU `virt` 的 `sifive,test1` / `sifive,test0`；`code == 0` 写入 `0x5555`（QEMU 退出码 `0`），非零 `code` 写入 `(code << 16) | 0x3333`（QEMU 以对应非零退出码终止）。

## 当前限制

- 所有平台表、设备/资源池、IRQ 表与驱动 bump arena 均为固定容量，无动态注销、卸载或热插拔。
- 暂不支持总线 `ranges` 地址转换、`interrupts-extended`、多 PLIC context/控制器路由及共享 IRQ 仲裁。
- virtio-mmio 等外设仅完成资源发现，尚无块设备驱动。
- 硬件轮询无超时保护；日志无级别过滤，内核 `LOG_*` 同步输出路径在关中断区间直接轮询串口硬件（用户态 `SYS_WRITE` 已走可调度 `CharDevice::write()` 阻塞路径）。
- RX 环仅 64 字节且无硬件流控；早期启动或硬件 FIFO 溢出丢字节不在 `rx_overrun_count()` 统计范围内。
