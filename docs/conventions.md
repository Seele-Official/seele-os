# 代码与文档约定

本文面向贡献者，约定命名、所有权、并发、错误处理及文档维护方式。修改已有代码时，
优先保持相邻模块的一致性；不要为统一风格而扩大改动范围。构建与 ABI 的硬边界见
[构建与运行](build.md)，这里不重复维护工具链和符号清单。

## 命名与类型形态

- **snake_case：像 C struct 一样使用的类型。** 它们必须是聚合：没有用户声明的构造
  函数、没有成员函数、没有虚函数、只有 public 数据。`proc::thread`、`proc::process`、
  `proc::kernel_context`、`boot::region`、`boot::hart`、`boot::info`、
  `device::mmio_range`、`device::irq_line`、`device::resource`、`driver::descriptor`、
  `fdt::cell_counts` 都属于这一类。
- **PascalCase：有行为的类。** 例如 `device::Device`、`mmio::Region`、
  `fdt::Tree`/`Node`/`Property`、`CharDevice`、`Spinlock`。有封装、成员函数、构造/
  析构或虚函数的类型通常保持 PascalCase。仿 `std::format` 的 `format_args`/
  `format_arg_store`/`basic_format_string` 是既有小写例外；新增有行为的类型优先
  PascalCase。
- **函数和成员函数用 snake_case。** 自由函数如 `proc::make_thread()`、
  `vm::k::create_process_pagetable()`；成员函数如 `Device::find_resource()`、
  `mmio::Region::read()`。动作命名用动词短语，容器接口沿用标准库的 `size`/`empty`/
  `front` 等名词。
- **变量和参数用 snake_case。** 布尔量优先用 `is_`/`has_` 前缀，计数用 `_count`，
  字节单位用 `_size`，地址用 `_base`/`_end`。地址类型明确写成 `Paddr`/`Vaddr`，
  不要用 `uint64_t` 代替。
- **命名空间小写。** 例如 `proc`、`boot`、`device`、`driver`、`fdt`、`mmio`、
  `trap`、`vm`、`heap`、`phymem`。嵌套命名空间用 `namespace k` 这样的短名，调用处
  写成 `vm::k::...`。
- **文件级状态沿用现有前缀。** 可变表/状态用 `g_` 前缀，例如 `g_devices`、
  `g_handlers`；启动期单例或地址空间对象沿用 `started`、`tree`、`k_pagetable` 这类
  现有命名。文件级常量用 `constexpr inline`，按现有风格可用 `kDrivers`/`kMatch` 或
  大写下划线名。
- **模板参数和 concept 用 snake_case。** 例如 `address_like`、`arg_source`。

成员变量有两种合法形态：

1. 类里有封装的成员用 `m_` 前缀，例如 `Device::m_node`、`Property::m_size`；
2. 像 C struct 一样的数据成员用普通名字，例如 `Section::start`/`end`。**普通命名的
   成员在成员函数里必须通过 `this->` 访问**，这样成员和局部变量/参数一眼可分：
   `this->end - this->start`。

`m_` 成员不需要 `this->`；不要为了统一而给 `m_` 成员再加 `this->`。

## C++ 核心准则

这个内核使用 `-fno-exceptions -fno-rtti`，没有宿主运行时，因此下列规则是硬约束：

- **不抛异常、不用 RTTI。** 不写 `throw`/`try`/`catch`、`dynamic_cast`、`typeid`。
  可恢复错误通过返回值、`std::optional` 或 `std::expected` 表达，调用方显式检查；
  不把错误访问触发的 `abort()` 或缺少实现的终止函数当成错误处理机制。
- **RAII 管理锁和资源。** 优先用 `Spinlock` + `LockGuard`，避免手写不成对的
  `lock()`/`unlock()`。正常锁临界区的中断嵌套由 `Spinlock` 维护，不要绕过它自行开启
  `sstatus.SIE`；需要检查条件再睡眠的路径，按调度接口要求显式维护外层中断状态。
  **不得持自旋锁调用 `sleep()`、`schedule()` 或 `yield()`**，具体契约见
  [线程与调度](scheduling.md)。
- **优先值语义和静态存储。** 平台对象、驱动状态和 sink 活到内核结束，放在 `.bss`/
  `.data` 或驱动 arena，用 placement new；不要为单个永久对象去映射堆页。
- **显式说明所有权与生命周期。** 裸指针本身不表达所有权。借用/回指（如
  `thread::proc`、`Device::m_object`）、侵入式链表和分配器返回值要按各自契约使用。
  当前启动创建的进程/线程存活到内核结束，没有销毁流程；这不代表堆没有 `delete`。
  新增动态资源时说明由谁释放、何时释放，不依赖隐含的跨模块约定。
- **能用 `constexpr` 就用。** 地址算术、大小类别、格式检查和布局断言优先在编译期完成。
  需要保证静态初始化的全局状态使用 `constinit`；它不表示只读，也不保证对象位于
  `.data`。只读性用 `const`/`constexpr` 表达，启动前读取的段位置另行确认。
- **不变量用 `static_assert` 钉住。** 结构偏移、槽位数、页表几何、启动栈布局改变时
  构建失败，而不是运行时才发现。
- **枚举用 `enum class`。** 需要紧凑存储时指定底层类型（如 `uint8_t`），不要用裸
  `enum` 或整数常量。
- **显式初始化。** 用 `{}` 初始化，成员给默认值；地址的空值用 `null_paddr()`，指针
  用 `nullptr`，不要依赖零初始化碰巧成立。
- **类型转换显式且局部。** 用 `static_cast`/`reinterpret_cast`；地址与整数、指针的
  互转优先走 `mm/addr.h`/`mm/vm.h` 的边界函数，其他局部位置要能说明为什么类型系统
  在这里帮不上忙。
- **接口标注意图。** 不应忽略的结果标 `[[nodiscard]]`；可恢复失败用 `bool`、空指针或
  带错误信息的值表达，并说明失败后是否留下部分修改。不返回的函数用 `[[noreturn]]`，
  不抛异常的路径标 `noexcept`。
- **并发原语显式表达内存序。** `std::atomic` 使用 `memory_order_relaxed`/`acquire`/
  `release` 时说明理由；启动期发布用 release/acquire，而不是默认顺序碰运气。
- **头文件自包含。** 用 `#pragma once`，只 include 直接使用的头；实现细节留在
  `.cc`，头文件暴露契约。

错误处理的分界：不可恢复的内核不变量违反用 `SEELE_ASSERT`/`panic`；可恢复的分配失败
返回 `false`/`nullptr`；调用方错误（大小超出堆类别、非法地址）也是断言，不要静默
修正。

## 注释

- 注释解释**局部意图、不变量和为什么**，不复述代码行，也不替代文档。
- **注释不得引用文档。** 不写“见 docs/...md”“见 README 某节”“见第 N 节”；跨模块设计
  属于文档，代码注释只描述当前文件和当前接口的契约。
- 头文件注释写接口契约：参数范围、所有权、失败语义、调用时机和并发要求；实现文件
  注释写本文件内不明显的机制、边界和取舍。
- 单位、范围、方向要写清楚：半开区间写成 `[begin, end)`，栈向下增长，偏移是字节
  还是页；不要留下“大概”“可能”的模糊承诺。
- 代码改变时同步改注释；过时注释比没有注释更危险。
- 不写“TODO: 以后优化”这类没有边界的占位；未实现行为应写进文档的“未实现/后续”
  列表，而不是散落在代码里。

## 文档规则

文档的目的不是复述代码，而是回答代码局部回答不了的问题：方向、跨模块契约、架构
理由、操作流程和约定。因此：

- 每篇文档开头说明它回答什么问题；README 只做入口，不重复子系统细节。
- **当前行为和未来计划分开。** 描述已实现行为的文档只写当前事实；未来设计单独用
  短的“未实现/后续工作”列表，不把目标设计写成已实现。
- **链接文件，不链接章节号。** 引用实现用相对路径指向源文件或文档文件；不要写
  “见某文档第几节”，章节会变，文件链接更耐用。
- **不重复代码注释。** 头文件已经写明的逐字段语义不在文档里再抄一遍；文档只保留
  跨模块的契约和取舍。
- 删除而不是堆积。内容合并后删除旧文件；同一件事只在一个地方解释。
- 中文行文，标识符、路径、代码块和单位保持原样。
- 行为或契约改变时同时更新对应文档和索引；新增文档要加入 README 的文档索引。

## 提交前

- C/C++ 用 `.clang-format` 格式化；Markdown/JSON/YAML/TypeScript 用
  `.prettierrc.json`（`pnpm exec prettier --write <files>`）。
- **代码变更：** 至少构建一次 Release，并启动 QEMU 验证受影响行为，不只确认有启动日志。
  改 ABI/libc 边界时核对最终 ELF 的 `nm -u`，并构建 Debug，步骤见[构建文档](build.md)。
- **布局与并发变更：** 改结构布局、页表几何或启动栈时，确认相关 `static_assert` 仍然
  成立并验证 Debug；改多 hart 共享路径时补多 hart 运行检查。静态断言不能替代运行测试。
- **纯文档变更：** 对照实际代码和任务定义核实命令、默认值与限制；检查 Markdown 相对
  链接目标，并对改动文件运行 prettier。不需要为修改措辞而机械重编译，但新增的操作步骤
  应尽可能实际验证。
- 提交说明写明改了什么、验证了什么，以及哪些路径没有验证；不要把“编译通过”写成
  “功能正常”。
