# 调试与诊断

本文说明日志格式化、断言与 panic 流程、帧指针栈回溯以及地址符号化方法。串口驱动与 panic 锁契约见 [platform.md](platform.md)，内存与线程栈边界见 [memory.md](memory.md)。

## 日志与格式化

[console.h](../src/kernel/console.h) 提供 `print()` / `println()` 与 `LOG_DEBUG`、`LOG_INFO`、`LOG_WARN`、`LOG_ERR`。日志无运行期级别过滤，Release 同样编译执行。

- **格式检查**：字面量格式串由 `consteval` 在编译期验证占位符数量、参数类型与格式说明符；运行期格式串经 `vprint()` / `vprintln()` 解析，非法占位符按原文本输出。支持整数（十进制及 `d`/`x`/`X`/`b`/`B`，可选 `#`、零填充与最大 255 宽度）、`bool` 及可转为 `std::string_view` 的字符串。
- **输出行为**：单次 `print()` / `println()` / `LOG_*` 在 console 锁内完成格式化，按 64 字节 staging 块写入 [`ConsoleSink::write()`](../src/kernel/char_device.h)，不会被其他正常写者打断；多次调用拼成的行不合并加锁。输出路径不调度，可用于启动代码、trap 与中断上下文（未挂接 sink 时丢弃）。队列背压与锁序细节见 [platform.md](platform.md)。

## Assert、Panic 与停机

接口与实现见 [panic.h](../src/kernel/panic.h) / [panic.cc](../src/kernel/panic.cc)：

| 宏                                    | 构建行为                        | 失败结果                                        |
| ------------------------------------- | ------------------------------- | ----------------------------------------------- |
| `SEELE_ASSERT(cond[, fmt, args...])`  | 全构建求值一次                  | 输出条件、位置、可选消息与调用栈，停住当前 hart |
| `SEELE_DASSERT(cond[, fmt, args...])` | `NDEBUG` 下为空语句，条件不求值 | Debug 失败时同 `SEELE_ASSERT`                   |
| `SEELE_PANIC(fmt, args...)`           | 调用即失败                      | 输出消息、位置与调用栈，停住当前 hart           |
| `SEELE_UNREACHABLE()`                 | 调用即失败                      | 经 panic 报告 `unreachable code reached`        |
| `SEELE_ASSUME(cond)`                  | 仅作编译器假设，不做检查        | 条件为假属未定义行为，无诊断输出                |

条件含顶层逗号时须加括号（如 `SEELE_ASSERT((std::is_same_v<A, B>))`）；运行期消息使用 `panic(const char*)` 或 `debug::vpanic()`；编译期不变量使用 `static_assert`。

典型 panic 输出：

```text
panic: assertion failed: <表达式>[: <格式化消息>]
  at <file>:<line> in <function>
stack trace:
  #0 0x0000000080001e04
  ...
```

panic 入口先关闭本 hart 的 `sstatus.SIE`（防止定时器或外部中断在报告过程中切走当前线程与栈），再调用 `console::enter_panic()` 屏蔽设备中断并使常规日志静默丢弃；随后 `panic()` 与 `stacktrace::print()` 通过专用无锁紧急输出通道（`console::print_emergency()` / `println_emergency()` → `ConsoleSink::write_polled()`）直接轮询输出，使已持锁代码、trap、中断及早期启动路径均可报告现场。报告完成后清空 `sie` 并循环 `wfi()` 停住当前 hart（不自动停止其他 hart）。

[src/libc/stdlib.cc](../src/libc/stdlib.cc) 将 `abort()` 转发为 `panic("abort() called")`。Debug 构建启用 `_GLIBCXX_ASSERTIONS`，freestanding libstdc++ 前置条件失败（如空 `optional`/`expected` 或 `std::get()`）会经此路径停机。内核代码统一使用 `SEELE_UNREACHABLE()` 替代会产生 UB 的 `std::unreachable()`。

## 帧指针栈回溯

构建保留帧指针并关闭 unwind tables（见 [CMakeLists.txt](../CMakeLists.txt)）。[stacktrace.cc](../src/kernel/stacktrace.cc) 按 RISC-V GCC 帧布局遍历：`[s0 - 8]` 为返回地址，`[s0 - 16]` 为调用者 `s0`。

- 仅在当前线程栈 `[base, base + size]`（线程尚未建立时使用本 hart 启动栈）内读取，要求 16 字节对齐，最多输出 `MAX_DEPTH = 32` 帧。
- `idle()` 归还启动栈后不再将其视为有效回溯范围。
- 汇编入口、无标准序言函数、尾调用优化、`switch_thread()` 边界或过渡窗口均会自然截断回溯链。

## 地址符号化

使用与本次运行严格一致的 `kernel.elf` 进行符号化：

```bash
pixi run riscv64-unknown-elf-addr2line -f -C -i -e build/kernel.elf 0x8000019a
```

`-f` 输出函数名，`-C` 还原 C++ 符号，`-i` 展开内联帧。栈中打印的是**返回地址**，由于存在 2 字节 RVC 压缩指令，需结合反汇编确定调用指令精确位置。

## 排查清单

1. **完全无输出**：核对 QEMU 串口参数、`platform::attach_early_console()` 与 `console::attach()` 挂接状态。
2. **触发 panic**：保留完整 `panic:`、`at file:line` 及全部地址，使用对应构建目录的 ELF 交叉核对 `addr2line` 与反汇编。
3. **仅见 `abort() called`**：从 `#0` 向上定位 libstdc++ 断言调用点。
4. **栈提前截断**：确认是否处于建线程前、经过 `switch_thread()`、存在尾调用或汇编入口。
5. **输出交织或行缺失**：核对是否已进入多 hart 并发 panic 路径。
6. **链接或启动异常**：按 [build.md](build.md) 核对 freestanding 边界并用 `nm -u` 检查未定义符号。

## 当前边界

- 无内核转储（不保存内存、CSR 或完整寄存器快照），现场依赖串口日志与对应 ELF。
- 预留的用户态 `trap_frame` 路径尚未接入同等的异常回溯输出。
- `kerneltrap()` 对未处理异常/未知中断直接打印 `cause`/`sepc`/`stval`/`sstatus` 并 panic，无恢复逻辑。
- 日志无级别过滤与持久化；栈回溯上限 32 帧，仅覆盖符合帧指针约定的 C/C++ 栈帧。
