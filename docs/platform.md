# 平台层：FDT、设备、驱动与外设

本文说明固件描述如何变成设备资源，以及驱动、trap、console 和调度器之间的契约。启动全貌见 [architecture.md](architecture.md)，地址映射见 [memory.md](memory.md)，线程切换见 [scheduling.md](scheduling.md)，日志与诊断见 [debug.md](debug.md)。

导航：[初始化](#平台初始化) · [FDT 与启动发现](#fdt-与启动发现) · [设备与驱动](#设备与驱动模型) · [MMIO](#mmio-访问契约) · [外部中断](#外部中断trap-与-plic) · [Console/UART](#console-与-uart) · [限制](#当前限制) · [开发注意事项](#开发注意事项)

## 平台初始化

boot hart 在 [kmain.cc](../src/kernel/kmain.cc) 中按以下顺序初始化：

1. 清 `.bss`、执行 `.init_array`，用 DTB 的 direct-map 地址构造文件级 `static fdt::Tree`。
2. `early::attach_console(tree)` 尽力挂接轮询 console；随后断言树有效。
3. `device::enumerate(tree)` 生成设备资源；`boot::parse(tree, dtb)` 收集内存、hart、timebase 和 `/chosen` 信息。
4. 初始化物理分配器、共享内核映射及 MMIO 映射，创建并启用进程页表，再创建 idle 进程/线程。
5. `driver::probe_all()` 依次运行 Core、Device、Late；当前分别由 Core 级 PLIC 和 Device 级 Ns16550 提供中断与 console。
6. 仅在已有 console 时创建 shell，随后以 `started.store(..., release)` 发布初始化结果。

secondary 以 acquire 等待 `started`，不重复平台发现或驱动 probe，只建立自己的进程页表和 idle。各 hart 进入 idle 后才安装 `stvec` 并开启定时器和外部中断。

**生命周期契约：**设备节点句柄依赖 `Tree`，名字和属性视图依赖 DTB；两者都必须长期有效。`boot::parse()` 优先将 DTB 自身记为 `Reserved`，避免其页被物理分配器回收。

## FDT 与启动发现

### FDT 字节与查询层

[fdt.h](../src/kernel/fdt/fdt.h)、[reader.cc](../src/kernel/fdt/reader.cc) 和 [tree.cc](../src/kernel/fdt/tree.cc) 提供无分配、只读查询，不负责设备绑定或资源策略。

- 构造时检查 magic、`totalsize`（最大 16 MiB）、版本 17、`last_comp_version <= 17`、结构块/字符串块边界及根节点起始 token。无效树返回空查询结果；**内核启动仍要求树有效**。
- `u32()` / `u64()` 要求属性长度精确匹配；`string()` 取首个 NUL 结尾字符串；`boolean()` 仅判断属性存在。
- `find()` 只接受绝对路径，路径分量可省略 unit address；不解析 `/aliases`。`by_phandle()` 遍历查找，`phandle` 优先于 `linux,phandle`。
- `enabled()` 只检查该节点自己的 `status`：缺省、`okay`、`ok` 为可用；并不继承父节点状态。
- `cells()` 沿父链查找 `#address-cells` / `#size-cells`，默认 2/2；`RegIterator` 只支持地址和大小分别为 1 或 2 个 cell，跳过长度为 0 的范围。`#size-cells = 0` 不属于其支持范围。
- reservation iterator 从 `off_mem_rsvmap` 读取大端 `(address, size)`，遇 `(0,0)` 结束；没有可用 reservation offset 时为空。

**解析边界：**这是基于 blob 声明长度的边界检查，不是完整的固件验证器。调用者必须提供可读的 blob；构造器在知道长度前先读取头部的前 8 字节。实现不完整验证块间关系或 reservation 终止符；缺少终止符时可能把 blob 内其他内容当作 reservation。`reg` 不做总线 `ranges` 地址转换，平台当前直接将其地址视作物理地址。

### 启动发现结果

接口与实现见 [boot/info.h](../src/kernel/boot/info.h)、[fdt_boot.cc](../src/kernel/boot/fdt_boot.cc)。`boot::info` 使用固定容量数组保存：

| 信息                      | 当前来源与处理                                                                                             |
| ------------------------- | ---------------------------------------------------------------------------------------------------------- |
| `Usable` 区域             | 遍历 `device_type = "memory"` 节点的所有 `reg` bank                                                        |
| `Reserved` 区域           | DTB 自身、memory reservation block、`/reserved-memory` 下 enabled 子节点的 `reg`、`linux,initrd-start/end` |
| hart                      | `/cpus` 下 enabled 的 `cpu` 节点：单独解码 `reg` 首地址，记录 `riscv,isa`                                  |
| `timebase_frequency`      | `/cpus/timebase-frequency`，缺失时尝试 root 同名属性                                                       |
| `cmdline` / `stdout_path` | `/chosen/bootargs` 和 `stdout-path`，原样保留                                                              |

内存区域按 base 排序，并跨整个结果表合并同 kind 的重叠/相邻范围；不同 kind 不合并，保留项由物理分配器从可用范围中扣除。区域回绕或容量溢出会告警并丢弃，不会自动扩容。内存树遍历受 `SEELE_FDT_MAX_DEPTH` 限制；区域和 hart 容量分别由 `SEELE_MAX_REGIONS`、`MAX_HARTS` 限制，构建参数见 [build.md](build.md)。

`parse()` 不判断实际 boot hart；`kmain()` 按正在启动的 hartid 标记 `hart.boot`。发现 timebase 不等于调度器使用它计算 tick，见 [调度文档](scheduling.md#当前-kerneltrap-分发)。

## 设备与驱动模型

### 设备枚举

[device.h](../src/kernel/platform/device.h) 对外提供 `Device` 及两类资源：

- `Mmio`：物理基址和长度。
- `Irq`：控制器设备 id、source number 和 trigger；trigger 为 0 表示未知。

`Device` 提供 compatible 匹配、资源查找、属性访问、probe 状态和 `object()`。固件描述与驱动对象分离：即使没有驱动，固件设备仍存在；成功 probe 后才借用一个 `device::Object*`，而不是将原始私有状态当作接口。对于固件节点，`Device` 只保存节点表下标，FDT 句柄、设备和资源均由 [device.cc](../src/kernel/platform/device.cc) 的静态数组持有；设备 id 是 `device::all()` 下标。

枚举先收集候选并核对容量，再创建全部设备，最后解码资源，以便 IRQ 能引用控制器设备 id：

- 从 root 深度优先遍历，跳过名为 `reserved-memory` 的子树；节点自身必须 enabled、compatible 非空且至少有一个资源才成为设备。普通 disabled 节点并不会阻止遍历其子节点。
- 每个受支持的非零长度 `reg` entry 产生一个 MMIO 资源；不进行总线地址转换。
- `interrupts` 使用沿父链查找的 `interrupt-parent`，只接受控制器声明的 1 或 2 个 `#interrupt-cells`（source，或 source + trigger）。不支持 `interrupts-extended`。
- parent phandle 在树中不存在、cell 数不支持或属性长度无效时，不生成 IRQ 资源；parent 节点存在但未成为 Device 时，保留 IRQ 资源并告警，`controller = UINT32_MAX`。
- 达到设备/资源容量时丢弃整个候选节点，而非保留部分资源；遍历深度也有限制。

QEMU virt 中的 serial、PLIC、CLINT、virtio-mmio 等可被枚举，但**枚举不代表已有驱动**。CPU 的裸地址 `reg` 不由 MMIO iterator 处理；启动发现另行解码它。

### 驱动匹配与 probe

[registry.h](../src/kernel/drivers/registry.h) 定义 descriptor，[table.cc](../src/kernel/drivers/table.cc) 是唯一驱动清单，[registry.cc](../src/kernel/drivers/registry.cc) 负责匹配、分配私有状态及挂接。

- probe 级别为 `Core → Device → Late`，级别内按 priority 降序、注册顺序执行。
- 最佳匹配在**全部描述符**中选择：priority 最大者优先，同值取注册靠前者；只有它能对 `Unbound` 设备 probe。每次仅调用一次；失败标记为 `Failed`，不重试，也不自动尝试另一个驱动。
- probe 返回 `device::Object*`：成功返回已构造、初始化好的对象，失败返回 `nullptr`。固件设备通过 `object()` / `set_object()` 保存类型化的借用指针；只有成功对象才能挂接子系统。
- `priv_size > 0` 从 1 KiB、16 字节对齐的静态 bump arena 取零初始化存储，否则传 `nullptr`。驱动负责 placement new；需要更高对齐的存储须另行安排。arena 不回收（包括失败 probe），用尽时告警并标记设备失败；probe 当前仅在 boot hart、secondary 释放前运行，不提供并发注册/分配保护。
- 核心对返回对象逐项查询 `trap::ExternalController` 和 `CharDevice`，分别调用 `trap::set_external_controller()` 和 `console::attach()`；查询不是互斥分支，一个对象可提供多个接口。控制器在 Core 级挂接完成后，Device 级 UART 才能按 IRQ 注册自己的 handler。

### 驱动对象与轻量接口查询

[device_object.h](../src/kernel/device_object.h) 定义抽象基类 `device::Object`、`device::Implements<Interfaces...>` 和 `device::dyn_cast<T>()`。具体驱动继承 `Implements` 声明提供的接口；`dyn_cast` 对空对象或未实现的接口返回 `nullptr`，const 对象只返回 const 接口。

类型身份使用每个接口对应的模板 inline 字节变量地址，不依赖 RTTI，也没有中央设备类型枚举。`Implements` 先通过 C++ `static_cast` 取得正确调整过的接口指针，再交给查询调用方；因此 `Object`、接口子对象和 arena 存储的地址无须相同，也没有“接口必须为第一个基类”的布局约定。核心只识别公共接口，不识别 PLIC 或 Ns16550 等具体类。对象基类和接口均不提供通过基类删除的路径；arena 对象和子系统借用的接口指针必须存活到内核结束。

当前只有 PLIC 和 Ns16550 两个 descriptor。新增驱动需增加实现、驱动表项和构建源文件，并核对 arena 容量、对齐、probe 顺序及失败后的资源状态。early console 不属于 probe 级别，不能依赖设备枚举、arena 或 console 锁。

## MMIO 访问契约

[mmio.h](../src/kernel/platform/mmio.h) 的 `mmio::map(resource)` 只接受非空 MMIO 范围并检查其不超出 `DIRECT_MAP_SIZE`，然后返回 direct-map 别名；**它不建立页表映射**。设备 MMIO 映射在 `vm::k::init()` 阶段建立。当前 `PAGE_OFFSET = 0`，别名为恒等地址；普通驱动应通过 `mmio::map()`，不自行转换 `Paddr`。

`Region::read<T>()` / `write<T>()` 执行单次 volatile 整数访问，类型不超过 8 字节。对齐（绝对地址）和越界仅由 Debug 断言检查，Release 下不是运行时保护。`Region::fence()` 当前发出 `fence rw,rw`；volatile 不代替硬件内存序，新增设备须核对所需的寄存器访问顺序、屏障和寄存器窗口，不能把 `map()` 成功视为这些条件已验证。

## 外部中断：trap 与 PLIC

### 分发接口

[trap.h](../src/kernel/trap.h) / [trap.cc](../src/kernel/trap.cc) 将控制器与设备分离：

- 控制器实现 `ExternalController::{handle(), enable(irq, priority)}`。
- 设备实现 `ExternalHandler::handle(irq)`，负责服务并消除设备中断条件。
- trap 仅借用对象指针；对象由驱动在私有存储中构造，必须活到内核结束。

Core 级 probe 返回控制器对象后，由驱动核心查询接口并通过 `set_external_controller()` 安装。设备随后调用 `register_external_handler()`；irq 为 0、重复、表满、无控制器或 `enable()` 失败均返回 `false`，不保留新表项。priority 为 0 表示登记但屏蔽。

`kerneltrap()` 收到 `EXT_INT` 后调用控制器 `handle()`；控制器 claim source，调用 `dispatch_external_handler()`，最后 complete，**即使未找到 handler 也必须 complete**。未登记 source 只对有限个不同 source 各告警一次；complete 本身不能消除设备的持续中断条件。服务结束后的调度属于 trap，不由设备 handler 直接切换线程。

handler 运行于中断关闭的 trap 上下文，不能睡眠；尤其 UART handler 不能递归记录日志。控制器与表项在启动期注册完成，由 `started` 发布；运行期注册和多个控制器路由均不受支持。

### 当前 PLIC 实现

[plic.cc](../src/kernel/drivers/irq/plic.cc) 取第一个 MMIO 资源，验证窗口能覆盖所用 context 及 `riscv,ndev` 的范围，清空该 context 的 enable 位图、设 threshold 为 0，再返回对象供核心挂接。`enable()` 写 source priority 并据其是否为 0 开关 enable 位；`handle()` 循环 claim/dispatch/complete，最多服务 source 数量次，避免单个持续中断源无限占据本次 trap。

**平台假设而非通用映射：**实现硬编码 PLIC context 1，按 QEMU virt 的 hart 0 S-mode context 使用；它不解析 `interrupts-extended`，也不按实际 boot hartid 查表。其他 context 没有在这里初始化或关闭，因此“中断只送 boot hart”还依赖固件/平台初态。当前只安装一个控制器，source 按全局编号查表；Ns16550 只使用 IRQ number，不利用资源的 controller id 或 trigger 做路由/编程。非 QEMU 布局、多控制器或改换接收 hart 都需补充协议。

## Console 与 UART

### 分层与挂接

[CharDevice](../src/kernel/char_device.h) 同时定义输出、panic hook 和阻塞 `read()`。[console.cc](../src/kernel/console.cc) 负责格式化、正常输出串行化、设备指针和 panic 路径选择；驱动负责硬件、收发队列及输入等待。console 不持有硬件细节，驱动不查询 console 的 panic 状态。

`console::attach()` 在锁内排空旧设备再替换指针；`attach_boot()` 是 early 阶段的无锁挂接。指针为空时输出被丢弃，`read()` panic。`read()` 只查找设备并转发，**不持 console 锁等待**。`CharDevice` 的 protected 非虚析构函数禁止通过接口删除；设备没有销毁路径。

成功 probe 返回的对象只要提供 `CharDevice`，就默认成为 console，不依据 `/chosen/stdout-path` 选择真实设备；多个匹配 UART 依 probe 顺序依次替换，最后挂接者成为 console。接口要求实现输入，尚无纯输出 console 类型。

### 输出与 panic 契约

| 操作             | 当前 Ns16550 行为                                                                                        |
| ---------------- | -------------------------------------------------------------------------------------------------------- |
| `write()`        | 中断模式入 128 字节 TX 环；环满时轮询排空再继续。轮询模式直接写硬件。不调度，但不保证立即返回            |
| `flush()`        | 轮询排空 TX 环，再等 `LSR.TEMT`，保证正常路径上 FIFO 和移位寄存器均空；需要锁，不可从本设备 handler 调用 |
| `write_polled()` | 无锁直接轮询写 THR，不操作共享队列，供 panic 使用                                                        |
| `flush_polled()` | 仅尝试获取设备锁；成功则尽力排空 TX 环，失败即返回；不等待 `TEMT`                                        |
| `enter_panic()`  | 先置设备闩锁，再屏蔽 UART IER；普通输出转轮询，handler 不再服务设备，读者触发 panic                      |

正常锁顺序是 console → 设备；输出不调用调度器。**环容量只限制单次队列排空的字节数，不是耗时上界**：硬件轮询没有超时，长输出可能多次排空，锁竞争也可能等待。

`console::enter_panic()` 先通知设备，再发布自己的闩锁，首次进入时调用 `flush_polled()`。设备闩锁使已经选择普通路径、稍后才进入驱动的写者也转为轮询。panic 不等待可能无法释放的锁，不保证排空已入队数据，也不串行化其他 hart；未提交输出可能乱序或丢失，并发轮询字符可能交织。它不等于可靠的全机停机/输出屏障。

### 输入与调度器契约

[Ns16550::read()](../src/kernel/drivers/uart/ns16550.cc) 的调用条件是：已有当前线程、`SIE=1`、不持自旋锁，且 **cpuid 必须为 0**。同一时刻仅支持一个等待 reader。

- 中断模式先取 RX 环字节；环空时还会尝试服务硬件 FIFO。仍为空则登记 reader 并 `shed::sleep()`，让 hart 运行其他 ready 线程或 idle。
- 从空队列检查、reader 登记到真正入 sleeping，始终保持本 hart 中断关闭；设备锁在睡眠前释放，避免丢失唤醒及持锁切换。恢复后 `read()` 自行重新开启 `SIE` 并重查条件。
- 接收 handler 将字节存入 64 字节 RX 环，**释放设备锁后**调用 `shed::wake(reader)`；trap 随后 `schedule()`。`wake()` 只查本 hart，因此读者和接收中断必须在同一 hart。
- 无可用接收中断时不进入 sleeping，而是在开中断后 `wfi()`，等待定时器等中断后重试轮询；这仍占用该线程的运行身份，不是调度器阻塞等待，也没有严格的输入延迟上界。
- panic 后的新 `read()` 触发 panic；已经睡眠的读者不保证再被唤醒。

调度原语的前提见 [scheduling.md](scheduling.md#调度与等待契约)。未来移动 shell 或中断目标 hart 时，必须同时调整驱动断言和本地 sleep/wake 协议，不能只修改 PLIC context。

### Early console

[early/firmware.cc](../src/kernel/early/firmware.cc) 复用 Ns16550，在静态存储中构造对象，不分配、不在发现路径中记录日志、不依赖 console 锁，也不注册中断，始终轮询输出。

优先使用 `/chosen/stdout-path` 的绝对路径（去掉 `:` 后选项）；路径不解析别名。没有找到目标节点时 DFS 回退到首个 enabled 的 `ns16550a` / `ns16550` 节点。**chosen 命中不检查 compatible 或 enabled**；命中但 `reg` 不可用时也不再回退。

early 路径将非法 `reg-shift` / `reg-io-width` 分别回退为 0 / 1，并检查基址对齐和窗口能覆盖寄存器 7。失败不致命；内核继续，直到有 console 挂接前的输出均丢弃。它直接构造 MMIO Region，不走普通设备的 `mmio::map()`；不可据此假设通用 MMIO 映射检查已执行。

### Ns16550 正式驱动

[ns16550.cc](../src/kernel/drivers/uart/ns16550.cc) 匹配 `ns16550a` / `ns16550`，Device 级、priority 0：

- 使用首个 MMIO 资源，读取 `reg-shift`、`reg-io-width` 和 `clock-frequency`；前两者非法时回退为 0 / 1。按 `SEELE_UART_BAUD`（默认 115200）设置除数、8N1、开启并清空 FIFO；时钟缺失或除数过小时用 1，除数过大时钳为 `0xffff`。
- 取首个 IRQ 资源以 priority 1 注册 handler，成功才启用接收中断；失败仍可 probe 成功，保持轮询模式。发送空中断只在 TX 环非空时开启，排空后关闭。
- 每次 handler 先收后发，各自最多处理一个探测出的 FIFO 深度（1 或 16 字节）。RX 环满仍读出硬件字节并丢弃，累计 `rx_overrun_count()`，不等待读者。
- 设备锁保护环、IER 和共享寄存器。正式 probe 没有 early 路径同等的完整窗口/对齐预检，不能把可绑定 compatible 当成寄存器布局已验证；尤其 Release 不含 MMIO 访问断言。

## 当前限制

- FDT、设备、资源、驱动排序、IRQ 表、私有状态均有固定容量，不支持动态热插拔。
- 当前缺少总线 `ranges` 转换、`interrupts-extended`、多 PLIC context/控制器映射和共享 IRQ 仲裁。
- virtio-mmio 等目前只有资源发现，没有 virtio 块设备驱动。
- 日志没有级别过滤；输出压力可能在中断关闭区间转为轮询等待，影响调度与接收服务。
- RX 环只有 64 字节，没有串口输入流控；早期启动、硬件 FIFO 或 RX 环积压均可能丢字节，`rx_overrun_count()` 仅统计驱动从硬件读出后因软件 RX 环满而丢弃的字节，不覆盖全部丢失。

## 开发注意事项

- 设备/DTB、驱动对象和注册项都是内核生命周期对象；没有注销、卸载或重新枚举时的引用失效处理。不要运行期重新调用枚举或 probe 来模拟热插拔。
- 新增设备需核对固定容量、寄存器窗口、锁顺序和失败处理，不能仅依赖 compatible 匹配或 `mmio::map()` 成功。超限行为需按具体接口检查。
- 新增平台先核对总线地址转换、中断控制器拓扑及接收 hart 归属；新增异步输出设备须保留普通路径不调度、panic 不等待锁的契约，且不应承诺没有实现的超时或交付保证。
