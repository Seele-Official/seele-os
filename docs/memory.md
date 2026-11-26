# 内存管理

本文说明地址类型、物理页池、Sv39 页表、内核堆以及启动栈与线程栈的契约。内存与 MMIO 发现见 [platform.md](platform.md)，切栈与上下文切换见 [scheduling.md](scheduling.md)，栈回溯见 [debug.md](debug.md)。

## 地址类型与虚拟布局

[addr.h](../src/kernel/mm/addr.h) 将 `Paddr` 和 `Vaddr` 定义为独立的 64 位强类型，通过 `paddr()` / `vaddr()`、`to_uint64()` 及 `to_pointer<T>()` 显式转换，禁止混用或裸整数替代。区间统一采用半开区间 `[begin, end)`，辅助函数详见头文件注释。

[vm.h](../src/kernel/mm/vm.h) 定义 `PAGE_OFFSET = 0`（恒等 direct map）与 `DIRECT_MAP_SIZE = 1ull << 37`（128 GiB）。`vm::k::P2V()` / `V2P()` 仅适用于 direct-map 区域内的物理页，**不得用于用户地址、高位内核堆或线程栈地址**。

| Sv39 根项        | 用途                                                                                                    |
| ---------------- | ------------------------------------------------------------------------------------------------------- |
| 0–127            | direct-map 窗口 `[0, 2^37)`（128 GiB）：映射内核镜像、启动栈及启动期发现的 RAM、保留区与 MMIO 页        |
| 128–255          | 进程私有用户虚拟空间 `[0x0000_0020_0000_0000, 0x0000_0040_0000_0000)`（128 GiB，`PTE_U=1`，每进程独立） |
| 256–262          | 共享内核堆（`HEAP_BASE = 0xffff_ffc0_0000_0000`）：7 个 1 GiB arena，每大小类别一个                     |
| 263–507、509–511 | 保留未用                                                                                                |
| 508              | 共享线程内核栈窗口（`KSTACK_WINDOW_BASE = 0xffff_ffff_0000_0000`，大小 1 GiB）                          |

## 物理页分配器

[kalloc.h](../src/kernel/mm/kalloc.h) / [kalloc.cc](../src/kernel/mm/kalloc.cc) 以 `Paddr` 分配和回收 **4 KiB 整页**：

- `phymem::alloc()` 耗尽时返回 `null_paddr()`，返回页不清零；空闲链受自旋锁保护，链表指针写入空闲页首部，归还后禁止继续访问。
- `phymem::init()` 由 boot hart 在首次分配前执行一次，将各 `Usable` 区间向内取整页并裁入 direct-map 范围；内核镜像、启动栈、页 0 以及与任何 `Reserved` 区间相交的整页均排除在外。保留区副本供后续 `free()` 校验（例如保护 DTB 所在整页）。
- `phymem::free(page)` 在所有构建中校验地址非零、页对齐、位于已记录的 `Usable` 范围内且不与镜像或保留区相交，违反即 panic。启动栈切出后可在满足上述条件时逐页归还。

## 内核堆

[heap.h](../src/kernel/mm/heap.h) / [heap.cc](../src/kernel/mm/heap.cc) 提供全内核共享的小对象分配器及全局 `operator new/delete`：

- `heap::alloc(size, align)` 选取可同时满足大小与对齐的最小类别：`16, 32, 64, 128, 256, 512, 1024` 字节（默认 `DEFAULT_ALIGN = 16`）。`align` 须为非零 2 的幂，且 `size` 与 `align` 均不超过 1024；非法请求一律 panic（含 `new (std::nothrow)`）。
- 每个类别独立持有空闲链锁与高水位指针；链空时向该类别预留的 level-1 子树尾部按需映射一页并切分为定长槽位。物理页、页表页或 arena 耗尽时 `heap::alloc()` 返回 `nullptr`（未映射成功的数据页会归还物理池）。普通 `new` 耗尽时 panic，nothrow 形式返回 `nullptr`。
- `heap::free()` 忽略 `nullptr`，校验非空指针落在堆窗口内且对齐到所属类别的已映射槽位（否则 panic），随后将槽位归还类别空闲链；类别直接由地址所在 arena 推导，无需对象头。
- 堆在物理池与共享子树初始化完成且当前 hart 启用分页后方可使用；早于分页启用的静态构造函数不得依赖堆。

## 页表建立与运行期更新

[riscv.h](../src/kernel/riscv.h) 与 [vm.cc](../src/kernel/mm/vm.cc) 实现 4 KiB 页 Sv39（不支持 superpage，`walk()` 遇到非叶子 R/W/X 项即失败），页表页通过 direct-map 别名读写。

### 启动期建立与启用

boot hart 在 [kmain.cc](../src/kernel/kmain.cc) 初始化物理池后，于分页关闭状态下执行 `vm::k::init()`：

1. 按 [linker.ld](../src/kernel/linker.ld) 页对齐边界映射内核镜像：`[TEXT.start, RODATA.start)`（R+X）、`[RODATA.start, DATA.start)`（R）、`[DATA.start, IMAGE.end)`（R+W）以及独立 `NOLOAD` 段 `.stack`（`BOOT_STACK`，R+W）。
2. 将 `Usable`/`Reserved` 内存区与设备 MMIO 裁入 direct-map、向外取整页合并后映射为 R+W 不可执行，跳过已有叶子以保留镜像权限。`Reserved` 仅表示不由物理池分配；`mmio::map()` 见 [mmio.h](../src/kernel/platform/mmio.h)，只借用此阶段建好的别名。
3. 为堆根项 256–262 与内核栈根项 508 各预建一张清零的共享 level-1 表，并回读校验镜像各权限组的首个叶子。

这张内核根页表由 `vm::k::pagetable()` 暴露，直接作为系统进程 `proc::kernel_proc()` 的根页表。**共享内核根项（0–127、256–262 与 508）在 `vm::k::init()` 后冻结**；后续调用 `create_process_pagetable()` 为新用户进程分配根表时，原样复制这些共享根项并保持根项 128–255 全零私有，运行期堆与内核栈扩展仅写入已共享的下级页表。

每个 hart 直接使用 `vm::k::pagetable()` 调用 `vm::k::enable()` 写入 `satp` 并执行本地 `sfence.vma`；secondary hart 在 acquire 读取 `g_started` 后执行相同步骤。跨进程切换时由 [switch.S](../src/kernel/sched/switch.S) 更新 `satp`（同一进程内的内核线程切换跳过 `satp` 写入）。

### 映射接口契约

- **`vm::mappages()`**：要求 `va`、`pa`、`size` 页对齐，`perm` 在 `PTE_LEAF_MASK` 内且合法（W 必带 R），禁止重复映射；调用方须保证虚拟与物理地址范围有效。中途页表页分配失败时返回 `false`，不回滚已写入项且不刷新 TLB。硬件 A/D 位更新依赖 [start.cc](../src/kernel/start.cc) 配置 `menvcfg.ADUE`。
- **`vm::k::mappages()`**：供共享堆与共享内核栈扩展使用，在共享页表锁下向已预留根项子树添加 R+W 不可执行映射，成功时执行本地 `sfence.vma`。
- **`vm::k::alloc_kernel_stack()` / `free_kernel_stack()`**：在共享根项 508 子树内按槽分配与回收带 guard 页的线程内核栈。
- **`vm::u::map_user_pages()` / `translate_user()` / `check_user_range()` / `copy_from_user()` / `copy_to_user()` / `destroy_user_mappings()`**：管理进程私有根项 128–255 内的 `PTE_U=1` 用户映射与安全拷贝。内核全程保持 `sstatus.SUM = 0`，跨特权级拷贝通过软件遍历进程页表并校验 `PTE_V | PTE_U` 与读写权限后经内核直映完成，非法或跨界用户指针直接返回失败而不触发内核异常。`destroy_user_mappings()` 仅释放根项 128–255 下的用户叶子物理页与私有页表页，绝不触碰共享内核根项。

## 启动栈与线程栈

- **启动栈（按 hart）**：[start.S](../src/kernel/start.S) 将硬件 hart 0 固定为 `cpuid = 0`，secondary 通过 `.data` 中的 `boot_hart_seq` 原子分配稠密 `cpuid` 并写入 `tp`（超限停机）。[start.cc](../src/kernel/start.cc) 在 `.stack` 中静态分配 `boot_stacks`（布局由 [stack.h](../src/kernel/boot/stack.h) 与编译期断言约束；默认 64 槽、boot 64 KiB、secondary 8 KiB，共 568 KiB）。各 hart 切入 `idle()` 线程栈后逐页归还自身启动栈；未使用的启动栈槽位不回收。
- **线程栈（全局共享窗口按槽分配）**：全系统共享 1 GiB 窗口 `[KSTACK_WINDOW_BASE, KSTACK_TOP)`，槽 `i` 栈顶位于 `KSTACK_TOP - i * KSTACK_SLOT_SIZE`。每槽由 `KSTACK_SIZE`（默认 16 KiB）映射页与下方 4 KiB 未映射 guard 页组成，共可容纳 `KSTACK_MAX_THREADS`（默认 52,428）个全局唯一线程栈。`vm::k::alloc_kernel_stack()` 在锁保护下优先 O(1) 弹出已归还槽位的栈首侵入式空闲链，链空时递增高水位并经 `vm::k::mappages()` 按页映射新槽；`free_kernel_stack()` 校验槽位对齐与已映射边界后将槽挂回空闲链（保持页面映射不撤销，避免跨 hart TLB shootdown）。[process.cc](../src/kernel/proc/process.cc) 中的 `proc::make_thread()` 取槽失败时 panic，并在线程对象中记录 `kstack` 供回溯使用。

## 当前限制

- 无跨 hart TLB shootdown：当前仅新增映射且不撤销共享页，跨 hart 访问新映射依赖其他 hart 未缓存旧翻译。
- 无引用计数、重复释放检测、物理页合并，或 `phymem::init()` 输入重叠去重。
- 内核堆不支持超过 1024 字节的对象分配，释放后不归还数据页、不撤销映射且高水位不回退。
- 无按需分页或栈溢出恢复路径；访问未映射 guard 页触发异常（内核态直接 panic，用户态由 `usertrap()` 终止故障线程）。
