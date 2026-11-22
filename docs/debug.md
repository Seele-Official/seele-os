# 调试与诊断

本文按“先保留现场、再定位地址、最后判断边界”的顺序说明内核诊断。串口设备和输出路径见 [platform.md](platform.md)；内存布局与线程栈见 [memory.md](memory.md)。

## 先确认日志能到哪里

[console.h](../src/kernel/console.h) 提供 `print()` / `println()` 和 `LOG_DEBUG`、`LOG_INFO`、`LOG_WARN`、`LOG_ERR`。日志没有级别过滤，Release 也会执行；高频日志会增加发送队列背压，不要把 `LOG_DEBUG` 当成可关闭的追踪系统。

格式串是字面量时由 `consteval` 检查：占位符数量、整数/字符串类型以及格式说明符都在编译期验证。运行时格式串只能传给 `vprint()` / `vprintln()`；它们用同一解析器再次解析，非法占位符按文本输出，不会静默变成另一种格式。支持整数（默认十进制，`d`/`x`/`X`/`b`/`B`，可选 `#`、零填充和最多 255 的宽度）、`bool` 和可转为 `std::string_view` 的字符串。

普通输出在 console 自旋锁内完成一次调用的格式化，并以 64 字节 staging 块交给 [`CharDevice::write()`](../src/kernel/char_device.h)；一次 `println()` 或日志调用的内容不会被其他正常写者插入，但多次 `print()` 拼成的行并不因此整体原子化。设备队列满时，驱动会在调用现场轮询发送端再继续接收；这会使输出变慢，具体等待取决于设备状态，不要据此作实时性承诺。该路径不调度，可从启动代码、trap 和中断处理器使用；未挂接设备时输出被丢弃。`console::read()` 是线程上下文的阻塞转发：没有设备会 panic，等待和唤醒由驱动负责。

## Assert、panic 和停机

宏和实现见 [panic.h](../src/kernel/panic.h) 与 [panic.cc](../src/kernel/panic.cc)：

| 宏                                    | 构建行为                                 | 失败结果                                        |
| ------------------------------------- | ---------------------------------------- | ----------------------------------------------- |
| `SEELE_ASSERT(cond[, fmt, args...])`  | 所有构建都求值一次                       | 打印条件、位置、可选消息和栈，然后停住当前 hart |
| `SEELE_DASSERT(cond[, fmt, args...])` | 定义 `NDEBUG` 时整条语句为空，条件不求值 | Debug 失败时同 `SEELE_ASSERT`                   |
| `SEELE_PANIC(fmt, args...)`           | 调用即失败                               | 打印消息、位置和栈，然后停住当前 hart           |
| `SEELE_UNREACHABLE()`                 | 调用即失败                               | 以 panic 路径报告 `unreachable code reached`    |
| `SEELE_ASSUME(cond)`                  | 不检查                                   | 条件为假是未定义行为，不产生诊断                |

消息格式使用和日志相同的字面量规则；运行时拼接的消息应调用 `panic(const char*)` 或底层 `debug::vpanic()`。宏条件含顶层逗号时加括号，例如 `SEELE_ASSERT((std::is_same_v<A, B>))`。编译期条件用 `static_assert`，不是运行时 assert。

典型输出：

```text
panic: assertion failed: <表达式>[: <格式化消息>]
  at <file>:<line> in <function>
stack trace:
  #0 0x0000000080001e04
  ...
```

进入 panic 时，panic 入口先关闭本 hart 的 `sstatus.SIE`，再调用 `console::enter_panic()`。关中断必须先行：否则定时器或外部中断会在报告过程中进入 trap 并可能调度到其他线程，当前 hart 不会停下来，回溯的栈也可能在继续遍历时被切走。`console::enter_panic()` 先通知设备并尽力排出此前已接受的队列字节，随后所有输出改用 `write_polled()` / `flush_polled()`；普通写者在取得 console 锁后还会再次检查 panic 闩锁，因此不会在闩锁发布后启动新的排队输出。panic 路径不依赖中断、调度或 console/设备锁，因此可从已持锁的代码、trap、中断和 `SIE=0` 的启动上下文报告。它只保证尽力输出：其他 hart 可能同时写入，字符可能交织，切换瞬间仍在格式化的行可能丢失。panic 不恢复执行；实现是关闭本 hart 中断、清空 `sie` 后循环 `wfi`，其他 hart 不会因此自动停止。

`abort()` 由 [libc](../src/libc/stdlib.cc) 转发为 `panic("abort() called")`。Debug 构建定义 `_GLIBCXX_ASSERTIONS`，freestanding libstdc++ 的前置条件失败可能只留下这条消息和栈，不一定带条件文本；`std::get()`、空 `optional/expected` 等失败也可能走同一路径。`std::unreachable()` 的工具链展开可能是 `ebreak`，而当前 `kerneltrap` 对未处理异常只记录后返回，故内核代码使用 `SEELE_UNREACHABLE()`。

## 取回调用栈

内核不依赖 `.eh_frame` 或 libgcc unwinder；构建保留帧指针且关闭 unwind tables（见 [CMakeLists.txt](../CMakeLists.txt#L190-L205)）。[stacktrace.cc](../src/kernel/stacktrace.cc) 沿 RISC-V GCC 当前帧布局读取：`[s0-8]` 是返回地址，`[s0-16]` 是调用者的 `s0`。它只在当前线程栈 `[base, base + size]` 内读取、要求 16 字节对齐，最多打印 `MAX_DEPTH = 32` 帧；尚未建立线程时才使用当前 hart 的启动栈切片。`idle()` 归还启动栈页后，回溯不会再把它当栈读取。

因此回溯是尽力的诊断，不是 ABI 契约：汇编入口或没有标准序言的函数会自然截断，尾调用可能消除帧，跨 `switch_thread()` 不会形成一条连续链，当前线程已更新但 `sp` 仍在启动栈的过渡窗口也可能无帧。栈损坏会提前终止或使地址检查失败，不应为了“补全”而放宽边界。

## 符号化地址

日志和栈打印的是链接时地址。使用与运行内核完全对应的 ELF：

```bash
pixi run riscv64-unknown-elf-addr2line -f -C -i -e build/kernel.elf 0x8000019a
```

`-f` 显示函数名，`-C` 还原 C++ 名字，`-i` 展开内联帧；`-g` 在构建中保留调试信息。栈里的值是**返回地址**，不是被调用指令的地址：应先定位到该地址附近，再结合反汇编确定调用指令长度（RISC-V C 扩展可能是 2 字节，不能机械地一律减 4）。函数末尾的返回地址也可能落在下一符号入口附近。重新链接或换构建目录后，旧地址不再可靠。

## 排查清单

1. **完全无输出**：确认 QEMU 串口参数、`early::attach_console()` 和后续 `console::attach()`；没有 sink 时早期失败可能只有内存中的停机，没有可见日志。
2. **出现 panic**：先保存完整的 `panic:`、`at file:line` 和全部地址，再用对应 ELF 反汇编/`addr2line` 交叉核对。
3. **只有 `abort() called`**：从 `#0` 向上定位 libstdc++ 调用点；它通常没有条件文本和源位置。
4. **栈过早结束**：检查是否处于建线程前、是否经过 `switch_thread()`、是否有尾调用或汇编序言；不要先假设是回溯器越界。
5. **日志互相交织或缺失**：确认是否已进入 panic 路径；并发 panic 输出允许交织，尚未提交的正常行可能丢失。
6. **链接/启动失败**：先按 [build.md](build.md) 核对工具链和 freestanding 边界，再用 `nm -u` 查未定义符号。

## 当前边界

- 没有用户态异常的完整诊断链：用户 `trap_frame` 恢复路径尚未提供同等的回溯输出。
- 没有内核转储；panic 不保存内存、CSR 或寄存器快照，现场依赖此前日志和 ELF。
- 日志没有级别过滤，也没有持久化；发送端停滞时背压可能长时间占用调用现场。
- 当前 `kerneltrap` 对未处理异常只记录并返回；`ebreak` 可能反复触发，因此调试代码应使用 panic 宏而不是依赖它停机。
- 栈最多 32 帧，且帧指针链只覆盖符合当前编译约定的 C/C++ 帧；打印结果不等于完整调用图。
