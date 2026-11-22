# Agent 工作指南

本文适用于本仓库内的编码代理，说明环境、命令入口和修改边界。开发命令以 [pixi.toml](pixi.toml) 为准，详细代码约定见 [开发约定](docs/conventions.md)。

## 开始工作

- 先确认工作目录并查看 `git status`、相关差异；保留用户已有的修改和暂存状态，不自动提交。
- 先读与任务相关的源码和文档，再修改；保持改动范围，不顺带重构或统一无关文件的风格。
- 搜索优先限定在源码、文档或工具目录。不要无差别遍历 `.pixi/`、`node_modules/`、`build*/` 或 `toolchain/`，这些目录含依赖、生成文件或独立仓库。
- [README](README.md) 提供项目和文档入口；涉及跨模块行为时先读 [系统架构](docs/architecture.md) 及对应子系统文档。

## 环境与命令：优先用 Pixi

**项目使用 Pixi 管理环境和任务。先看任务定义，已有任务就直接调用，不要自行拼装等价流程。** 所有下列命令都在项目根目录执行。

| 目的               | 命令                        |
| ------------------ | --------------------------- |
| 安装环境依赖       | `pixi install`              |
| 安装脚本依赖       | `pixi run setup`            |
| 配置默认构建目录   | `pixi run configure`        |
| 构建内核           | `pixi run build`            |
| 格式化和提交前检查 | `pixi run format`           |
| 单 hart 启动       | `pixi run qemu`             |
| 多 hart 启动       | `pixi run qemu 4`           |
| 指定构建目录启动   | `pixi run qemu 1 build-alt` |
| 单独生成测试磁盘   | `pixi run disk`             |

- 不要直接使用宿主的编译器、CMake、Ninja、clang-format、pnpm 或 QEMU 来替代项目入口，不手工拼接 `.pixi/envs/.../bin` 路径，也不为已有工具另装一套依赖。
- 只有现有任务没有覆盖所需操作时，才使用 `pixi run <工具> ...`；例如显式配置 Debug：

    ```bash
    pixi run cmake --preset qemu-virt -B build-debug -DCMAKE_BUILD_TYPE=Debug
    pixi run build build-debug
    ```

- `configure`、`build`、`disk`、`clean` 的位置参数为 `[buildDir]`，默认 `build`；`qemu` 的位置参数为 `[harts] [buildDir]`，默认 `1 build`。
- `configure` 仅在没有 CMake cache 时配置；变更已有配置须显式调用 Pixi 环境中的 CMake。
- `qemu` 自动依赖 `setup`、`build`、`disk`；不要手工重复编排这些步骤。
- `disk` 和 `qemu` 会重建对应构建目录的 `disk.img`。`clean` 会删除整个目标构建目录，运行前必须确认路径，不能拿项目根目录或用户数据目录当构建目录。
- `toolchain/` 是独立的 `seele-toolchain` 仓库，当前仓库消费其本地 channel。不为普通内核改动修改工具链、升级依赖或重写 lock 文件；环境问题按 [构建与运行](docs/build.md) 排查，缺少前置条件时如实说明。

## 格式化

- **统一入口是 `pixi run format`**，它运行 `pre-commit run --all-files`。不要先枚举所有 C/C++ 文件再逐个调用 clang-format，也不要重建一套格式化脚本。
- hook 和文件范围以 [pre-commit 配置](.pre-commit-config.yaml) 为准。当前包括通用文件检查、C/C++ clang-format、Python Ruff 和 TypeScript Prettier；Prettier hook 当前只匹配 TypeScript，并不自动格式化 Markdown、JSON 或 YAML。
- `--all-files` 按 Git 跟踪文件选择文件，不要为让检查发现新文件而擅自暂存。新增文件需要检查时可用 `pixi run pre-commit run --files <files>`；hook 未覆盖的文档格式化可用 `pixi run pnpm exec prettier --write <files>`。
- hook 可能自动修改文件，并以非零状态退出；先读结果和差异，确认修正后重新运行。若工具或依赖出错，应排查原因，不通过绕过 hook 或更换宿主工具掩盖失败。
- 全量检查若改到任务无关文件，保留用户原有修改，不把无关格式化混入交付。

## 内核修改约束

- 这是 RISC-V 64 位、QEMU `virt` 上的 C++26 freestanding 内核。不依赖宿主 libc、libstdc++ 或完整 C++ 运行时，不使用异常或标准 RTTI。
- 保持地址、所有权和生命周期边界；永久设备对象优先使用静态存储或驱动 arena。不随意引入堆分配、析构路径或新的运行时依赖。
- 不得持自旋锁睡眠或调度；修改共享状态、中断、页表或启动发布路径前，核对对应并发契约。
- 命名、类型形态、注释与文档要求遵循 [开发约定](docs/conventions.md)，不在此重复维护。行为改变时同步更新相关注释和子系统文档；中文行文，代码标识符保持原样。

## 验证与交付

- **目前没有确定的内核自动化测试方案。除非用户明确要求，不新增测试代码、宿主测试、测试框架或临时自动化启动脚本。** 不为本次任务另建测试设施。
- 代码修改至少运行 `pixi run build`，按受影响行为进行必要的手动 QEMU 检查；不能用“编译通过”代替行为验证。基础交互为提示符后的 `help`、`echo hello`、`info`。
- ABI、运行时或布局改动按 [构建与运行](docs/build.md) 检查 Debug 和 ELF 未定义符号；多 hart 共享路径改动需要多 hart 检查。相关命令仍通过 `pixi run` 执行。
- QEMU 没有启用 `Ctrl-A X` 退出，shell 的 `Ctrl-C` 仅取消当前输入行。管理并终止自己启动的具体进程，不批量杀死其他 QEMU 实例。
- 纯文档改动无需重编译内核；核实命令与链接，运行相应格式化和差异检查即可。
- 结束前审查最终 diff，执行 `git diff --check`。交付说明改动内容、实际执行的检查、失败原因及未验证项；不要声称运行过没有执行的检查。
