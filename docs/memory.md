# 内存管理

- **分配内存**：小对象用[内核堆](#分配小对象)，整页用[物理页池](#分配和归还物理页)。
- **判断地址能否访问**：先区分[地址类型和虚拟窗口](#判断地址能否访问)，再看[页表何时建立](#启动时建立并启用页表)。
- **修改映射或排查栈故障**：看[页表更新的契约](#修改页表)和[两种栈的生命周期](#区分启动栈和线程栈)。

FDT 如何提供 RAM、保留区和 MMIO 见 [平台与设备](platform.md)；线程如何切栈见 [调度](scheduling.md)，回溯见 [调试](debug.md)。

## 判断地址能否访问

[地址类型](../src/kernel/mm/addr.h)把 `Paddr` 和 `Vaddr` 定义为不同的 64 位枚举类型。代码用 `paddr()` / `vaddr()` 从整数显式构造，用 `to_uint64()` 取值；指针通过 `vaddr(const void*)` 和 `to_pointer<T>(Vaddr)` 转换，没有对应的 `paddr(const void*)` 重载。类型转换不检查地址是否可访问。

地址可加减字节偏移；同类地址相减得到 `uint64_t`，逆序相减会回绕。地址不能相加，异类地址不能相减。范围采用 `[begin, end)`：`range_fits()` 检查末端能否表示，`range_end()` 会饱和到 `UINT64_MAX`；`align_up()` 若越过 64 位地址空间顶端则回绕到 0，使用不可信范围时须留意。对齐和交叠辅助函数不检查映射。

[布局定义](../src/kernel/mm/vm.h)中的 `PAGE_OFFSET = 0`，所以 direct map 是恒等映射。`DIRECT_MAP_SIZE = 1ull << 38` 是布局上限，不表示整个 `[0, 2^38)` 都已映射。`vm::k::P2V()` / `V2P()` 只加减偏移，不查页表；已启用分页时，只有建好映射的别名才能访问。不要对堆指针或高位线程栈地址使用 `V2P()`，即使这些页另有 direct-map 别名。修改 `PAGE_OFFSET` 需要同时审视链接地址、镜像映射和高位窗口，不能只改常量。

| Sv39 根项        | 用途                                                                                           |
| ---------------- | ---------------------------------------------------------------------------------------------- |
| 0–255            | direct-map 窗口 `[0, 2^38)`；映射镜像、启动栈及启动时收集到的 RAM、保留区和 MMIO 的可覆盖页    |
| 256–262          | 共享堆，`HEAP_BASE = 0xffff_ffc0_0000_0000`；7 个 `HEAP_ARENA_SIZE = 1 GiB` 的 arena，每类一个 |
| 263–507、509–511 | 当前未使用                                                                                     |
| 508              | 每进程私有的线程栈映射，`KSTACK_WINDOW_BASE = 0xffff_ffff_0000_0000`，窗口 128 KiB             |

## 分配和归还物理页

[接口](../src/kernel/mm/kalloc.h)和[实现](../src/kernel/mm/kalloc.cc)以 `Paddr` 交接 **4 KiB 整页**。`phymem::alloc()` 耗尽时返回 `null_paddr()`（物理地址 0），不清零；需要零页的调用方自行清零，例如创建页表时。空闲链表受自旋锁保护，链表指针直接写入空闲页开头，归还后不得继续使用该页。

`phymem::init()` 由 boot hart 在首次分配或归还前执行一次；已经到达的 secondary 等待启动发布，不会参与分配。初始化把 `Usable` 区间向内取整页，跳过空区间或末端溢出的区间，超出 direct map 的尾部会裁掉（对越界的对齐末端告警）。镜像、启动栈、页 0 和接触已记录 `Reserved` 区间的整页都不入链。保留区有副本，供日后的 `free()` 检查；例如 DTB 即使只占一页的尾部，这一整页也不能分配，否则指向 DTB 的 `string_view` 可能失效。

`phymem::free(page)` 在所有构建中检查：地址非零且页对齐，位于初始化记录的整页 `Usable` 范围内，不属于镜像，也不与保留区相交；违反则 panic。启动栈不是永久保留区，切栈后可在满足这些条件时归还。这些检查不证明调用方拥有该页：分配器不做引用计数、页合并或重复释放检测。若另行构造初始化输入，不能传入覆盖同一整页的多个 `Usable` 区间；分配器不自行去重。

## 分配小对象

[堆接口](../src/kernel/mm/heap.h)与[实现](../src/kernel/mm/heap.cc)服务全内核，不是每进程的堆，也不是通用 `malloc`。`heap::alloc(size, align)` 选能同时容纳大小和对齐的最小类别：16、32、64、128、256、512、1024 字节。默认对齐 `DEFAULT_ALIGN = 16`；`align` 必须为非零的 2 的幂，且大小、对齐均不得超过 1024。非法请求 panic，即便使用 `new (std::nothrow)` 也一样。例如 `alloc(16, 64)` 占一个 64 字节对象。

每个类别锁住自己的空闲链和映射高水位。链空时，堆向该类别的 arena 尾部映射一页，再切成固定大小的对象。启动时已为每个 arena 预留 level-1 表，实际对象页按需分配。物理页或页表页不足、或该 arena 无法再增长时，`heap::alloc()` 返回 `nullptr`；新数据页若未映射成功，会归还物理页池。普通 `new` 遇到耗尽则 panic（不抛 `std::bad_alloc`）；nothrow 形式返回 `nullptr`。`delete` 和 `delete[]` 转发给 `heap::free()`。

`heap::free(nullptr)` 不做任何事。非空地址必须落在堆窗口内，并对齐到所属类别的已映射对象槽，否则 panic。类别由地址所在的 arena 推出，不需要对象头或传入大小，也不能检查指针是否真的已分配或重复释放。释放只回收对象，不还数据页、不撤销映射，高水位不回退。

堆没有独立的 `init()`，但必须等物理池和共享子树建好，且当前 hart 开启了使用这些子树的分页后再使用。启动路径的 `proc::make_proc()` / `proc::make_thread()` 在这之后分配对象；更早运行的静态构造函数不能依赖堆。

## 启动时建立并启用页表

按 [启动代码](../src/kernel/kmain.cc) 的顺序，boot hart 先初始化物理页池，再由 [`vm::k::init()`](../src/kernel/mm/vm.cc#L424-L500) 在分页关闭时分配并清零共享内核根表：

1. 按[链接脚本](../src/kernel/linker.ld)的页对齐边界映射内核镜像。`Section` 表示 `[start, end)`；三组映射为 `[TEXT.start, RODATA.start)`（R+X）、`[RODATA.start, DATA.start)`（R）和 `[DATA.start, IMAGE.end)`（R+W，含 `.data`、`.init_array`、`.bss`）。`IMAGE.end` 已向上取到整页，`.stack` 是其后的独立 `NOLOAD` 段，通过 `BOOT_STACK` 单独映射为 R+W。
2. 对启动内存区域（`Usable` 和 `Reserved`）以及设备 MMIO，分别把范围裁在 direct map 内、向外取整页并排序合并，以 R+W、不可执行映射。后续遍历跳过已有叶子，保留镜像等先建好的权限。`Reserved` 只表示不由物理池分配，不是禁止映射；[`mmio::map()`](../src/kernel/platform/mmio.h#L91-L108) 则只借用已建好的别名，不新建 PTE；只要资源有一部分越出 direct-map 窗口，就拒绝整个资源。
3. 为堆根项 256–262 各链接一张清零的 level-1 表（V 置位、无 R/W/X），但不预建栈根项 508。启动代码回读检查镜像各非空权限组的首个叶子 R/W/X 位。

构建使用 `boot::MAX_REGIONS` / `device::MAX_RESOURCES` 大小的固定数组；默认分别为 32 和 128（见 [CMake 配置](../CMakeLists.txt#L73-L79)）。正常启动的发现代码按上限收集，映射函数本身没有越界截断保护，改用其他输入时也要遵守该容量。物理页或页表页分配失败会导致启动 panic。

[`create_process_pagetable()`](../src/kernel/mm/vm.cc#L502-L541) 为每个进程新建根表，原样复制 0–262（不复制子树），并为根项 508 单独预建清零的 level-1 表，此时不分配线程栈数据页。这样各进程共享 direct map 和堆，但同一高位栈地址可以映射到不同的物理页。根项在构建完成后保持不变：改动共享根项不会更新已经复制的进程根项；在其指向的共享子树内新增映射则不需要复制根项。

每个 hart 用自己的 idle 进程根表调用 `vm::k::enable()`：检查非零、页对齐且物理地址落在 direct-map 窗口内，写入 `satp = MODE_SV39 | (pagetable >> PGSHIFT)`，再执行本地 `sfence.vma`。共享内核根表并不直接加载到 `satp`。secondary 等 boot hart 通过 `started` 的 release/acquire 发布启动状态后，才建立并启用自己的根表。启用后 `satp` 仍可能被[切换汇编](../src/kernel/shed/switch.S#L59-L71)写入；同进程线程切换则跳过该写入。

## 修改页表

[Sv39 常量](../src/kernel/riscv.h)和[页表实现](../src/kernel/mm/vm.cc)使用 4 KiB 页（`PGSIZE = 4096`、`PGSHIFT = 12`）、512 个 64 位 PTE 一张表。`PX<level>()` 从虚拟地址取每级 9 位，level 2 是根，level 0 是叶子。`PA2PTE()` 把物理页号移到 `PGFSHIFT = 10` 起始的 PPN 字段；`PTE2PA()` 只解出 44 位 PPN。软件通过页表物理页的 direct-map 别名读写表。`walk()` 拒绝带 R/W/X 的有效中间项（不支持 superpage）；允许分配时把缺失的表清零后链接，不填叶子。

- **通用 `vm::mappages()`**：要求 `va`、`pa` 和 `size` 按页对齐；`perm` 至少包含一个 R/W/X，W 必须带 R，且不能超出 `PTE_LEAF_MASK`（R/W/X/U/A/D）。对齐、权限和重复映射是硬检查；调用方仍须保证虚拟范围合法、物理范围在 direct map 内，不能依赖 `PA2PTE()` 的调试断言代替校验。中途页表分配失败返回 `false`，不回滚已装入的叶子和中间表，也不替调用方刷新 TLB。现有内核映射依赖[启动代码](../src/kernel/start.cc#L137-L138)设置 `menvcfg.AUDE` 后由硬件更新 A/D；内核没有按需分页，也没有实际启用的用户地址空间。
- **共享堆映射 `vm::k::mappages()`**：要求非空、不回绕的整页虚拟范围，所经根项必须已有效且无 R/W/X。这个检查不是堆窗口边界检查；当前唯一运行时调用者是堆扩展。它在共享页表锁下添加 R+W、不可执行映射，成功才执行本地 `sfence.vma`。失败同样不回滚；堆实际每次只映射一页，因此失败时该数据页没有装入叶子。
- **私有线程栈 `vm::k::map_thread_stack()`**：只在该进程已预留的 508 子树内添加映射；见下节。

目前没有跨 hart 的 TLB shootdown。当前路径只新增映射，共享堆也不撤销页；在别的 hart 上使用新增地址，依赖它此前没有访问该地址并缓存旧翻译。这不是 `sfence.vma` 提供的跨 hart 保证。

## 区分启动栈和线程栈

**启动栈按 hart 分配。** [`_entry`](../src/kernel/start.S) 在尚无栈时按硬件 hartid 分支：hartid 0 是 boot hart，`cpuid = 0`；secondary 通过 `.data` 中的 `boot_hart_seq` 原子分配稠密 `cpuid`，把它写进 `tp`，原 hartid 仍由 `a0` 传给启动代码。计数器不随 boot hart 清 `.bss` 被重置；`cpuid >= MAX_HARTS` 时停在 `wfi` 循环。

[`boot_stacks`](../src/kernel/start.cc#L67-L110) 是 `.stack` 中的一个静态对象，布局由 [栈参数](../src/kernel/boot/stack.h)和链接断言共同约束。`boot::STACK_COUNT == MAX_HARTS`，默认 64 个槽，`SEELE_BOOT_STACK_SIZE = 64 KiB`、`SEELE_SECONDARY_STACK_SIZE = 8 KiB`，共 568 KiB 的静态栈池。每段由整页组成、经恒等映射访问；hart 切到 idle 线程栈后才逐页归还自己的启动栈。未使用的槽不自动回收。

**线程栈按进程划槽。** 每个进程使用同一组虚拟地址 `[KSTACK_WINDOW_BASE, KSTACK_TOP)`；槽 0 居顶，槽 `i` 的栈顶是 `KSTACK_TOP - i * KSTACK_SLOT_SIZE`。每槽映射 `KSTACK_SIZE`（`SEELE_KSTACK_SIZE`，默认 16 KiB），正下方留 `KSTACK_GUARD_SIZE = 4 KiB` 不映射。`KSTACK_SLOT_SIZE` 默认 20 KiB，`KSTACK_MAX_THREADS = KSTACK_WINDOW_SIZE / KSTACK_SLOT_SIZE` 默认 6，与 hart 数无关。增大线程栈会减少槽位；构建检查窗口对齐、槽位落在同一个 level-1 项覆盖范围内、并至少容纳两个线程。栈向下增长，访问 guard 页会触发缺页，但 guard 不是任意越界的检测，也没有专门的栈溢出恢复处理。

[`proc::make_thread()`](../src/kernel/proc/process.cc#L57-L80) 先用 `thread_count.fetch_add(1)` 取槽，越限则 panic，再逐页分配并映射 R+W、不可执行的栈页，物理页不必连续。栈页或页表页不足时 panic，不会回滚并继续创建线程；成功后执行本地 `sfence.vma`，然后创建线程对象，记录 `kstack.base` 和 `kstack.size` 供回溯使用，不在该对象中存各栈页的 `Paddr`。
