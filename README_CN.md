# Atlas Engine

Atlas 是一个基于 **C++20** 与 **C# (.NET 9)** 的分布式 MMO 服务器框架。它采用类似 BigWorld 的 Login、Base、Cell、Database 与 Manager 多进程拆分，同时通过嵌入式 CoreCLR 运行托管玩法逻辑。

**[English](README.md)**

## Atlas 为什么存在

Atlas 面向大型、长线在线世界：服务器需要跨进程扩展，模拟权威需要留在服务端，玩法团队仍然需要用 C# 高效迭代。C++ 核心负责网络、进程协调、持久化、空间模拟与性能分析；C# 层负责实体行为、共享玩法契约、生成式客户端/服务端类型和示例玩法逻辑。

## 当前能力

- 通过 `machined` 做服务发现的多进程集群布局。
- Base/Cell 实体模型、Mailbox RPC 与生成式 C# 实体代码。
- CellApp 空间模拟，包括基于 Witness 的 AoI、Ghost 实体、BSP 分区与 offload 协调。
- 面向服务端玩法脚本的嵌入式 .NET 9 运行时。
- C# 客户端运行时，提供桌面与 Unity 集成表面。
- 运行时加载的 XML 与 SQLite 数据库插件。
- Tracy instrumentation、基线压测驱动与 trace 对比工具。

MySQL、DBAppMgr 与 Reviver 已在代码树中占位，但还不是完整的生产功能。

## 架构

```text
Client
  │
  ▼
LoginApp ──► BaseAppMgr ──► BaseApp ◄──► CellApp
                         │               │
                         ▼               ▼
                       DBApp         CellAppMgr
                         │
                    XML / SQLite
```

| 进程 | 职责 |
|---|---|
| `machined` | 本机服务注册、心跳目标与 Birth/Death 通知中心。 |
| `LoginApp` | 面向客户端的登录网关。 |
| `BaseAppMgr` | BaseApp 注册、选择与集群协调。 |
| `BaseApp` | Base 实体所有权、客户端代理状态、Mailbox 路由、持久化转交与脚本运行。 |
| `CellAppMgr` | CellApp 注册、空间几何、BSP 分区与 offload 协调。 |
| `CellApp` | 空间实体、移动、Witness 更新、AoI、Ghost 与脚本运行。 |
| `DBApp` | 通过配置的数据库后端异步持久化实体。 |
| `EchoApp` | 用于验证框架连线和构建产物的最小服务器进程。 |

服务器进程共享一套小型框架层级：

```text
ServerApp
├── ManagerApp     不带 CoreCLR 的守护/管理进程
└── ScriptApp      ServerApp 加嵌入式 CoreCLR
    └── EntityApp  ScriptApp 加实体定义与后台任务
```

`ServerApp` 提供配置、machined 注册、watcher 指标、tick 回调与信号分发。`BaseApp` 和 `CellApp` 通过 `EntityApp` 扩展它，使 C# 玩法运行在 C++ 服务器核心之上。

## 环境要求

Windows：

- Visual Studio 2022，安装 Desktop development with C++ 工作负载
- CMake 3.28+
- .NET 9 SDK
- Python 3.9+
- Git

Linux：

- GCC 13+ 或支持 C++20 的 Clang
- CMake 3.28+
- debug 预设需要 Ninja
- .NET 9 SDK
- Python 3.9+
- Git

Windows 构建包装脚本会自动加载 MSVC 环境。debug 预设需要 Ninja 时，包装脚本会自动准备。

## 构建

日常开发优先使用包装脚本：

```bash
# Windows
tools\bin\build.bat debug
tools\bin\build.bat release
tools\bin\build.bat profile

# Linux / macOS / Git Bash
tools/bin/build.sh debug
tools/bin/build.sh release
tools/bin/build.sh profile
```

常用选项：

```bash
tools\bin\build.bat debug --clean
tools\bin\build.bat debug --config-only
tools\bin\build.bat debug --build-only
```

也支持直接调用 CMake：

```bash
cmake --preset debug
cmake --build build/debug --config Debug
```

| 预设 | 用途 |
|---|---|
| `debug` | 快速迭代，使用 Ninja Multi-Config、Debug、MSVC `/Z7`、PCH 与测试。 |
| `release` | 优化构建，禁用测试。 |
| `hybrid` | RelWithDebInfo，用于带符号的优化调试。 |
| `profile` | RelWithDebInfo，启用 Tracy 与 profiling helpers，禁用测试。 |

支持的平台还提供 sanitizer 预设：`asan`、`asan-msvc`、`tsan`、`ubsan`。

## 测试

```bash
cd build/debug
ctest --build-config Debug --label-regex unit --output-on-failure
ctest --build-config Debug --label-regex integration --output-on-failure
ctest --build-config Debug -R test_math --output-on-failure
```

C# 测试：

```bash
dotnet test tests/csharp
```

提交 C++ 修改前，运行单元测试并对改动的 C++ 文件执行 clang-format：

```bash
clang-format --dry-run --Werror <changed files>
```

## 运行本地集群

最小构建验证：

```bash
./bin/debug/atlas_echoapp
```

开发集群启动顺序：

```text
machined → BaseAppMgr → CellAppMgr → DBApp → BaseApp → CellApp → LoginApp
```

在 Linux 或 Git Bash 下，集群包装脚本会通过 0 客户端的 world-stress 驱动启动同一套进程，并保持运行直到被中断：

```bash
tools/bin/run_cluster.sh
```

脚本化集群验证：

```bash
tools/bin/test_cluster.sh
# Windows
tools\bin\test_cluster.bat
```

## 数据库

Atlas 通过 database factory 以插件形式加载数据库后端。XML 是轻量本地后端；SQLite 提供单文件关系型后端，支持 WAL 与 busy-timeout。MySQL 选项已经存在，但当前代码树中的实现尚未完成。

DBApp 配置示例：

```json
{
  "database": {
    "type": "sqlite",
    "sqlite_path": "data/atlas_dev.sqlite3",
    "sqlite_wal": true,
    "sqlite_busy_timeout_ms": 5000
  }
}
```

## C# 玩法与客户端运行时

托管代码位于 C# 项目树中，并在 `ATLAS_BUILD_CSHARP=ON` 时随 native server 一起构建。`Atlas.Shared` 承载共享契约，`Atlas.Runtime` 将服务端玩法绑定到引擎，`Atlas.Client` 驱动客户端实体状态，`Atlas.Generators.Def` 将实体定义转成强类型代码。

示例玩法项目覆盖 base、client 与 stress 场景。

Unity 接入会为 Unity 工程准备 Atlas 托管客户端程序集和 native `atlas_net_client` 插件。

```bash
tools/bin/setup_unity_client.sh --unity-project <path>
# Windows
tools\bin\setup_unity_client.bat --unity-project <path>
```

## Profiling 与压测

`profile` 预设启用 Tracy instrumentation 与 profiling helpers。默认基线为 200 客户端、120 秒，捕获文件写入 `.tmp/prof/baseline/`。

```bash
tools\bin\build.bat profile
tools/bin/run_baseline_profile.sh
```

对比 CellApp 捕获：

```bash
python tools/profile/compare_tracy.py \
  .tmp/prof/baseline/cellapp_<old>.tracy \
  .tmp/prof/baseline/cellapp_<new>.tracy
```

更深入的背景可从 `docs/` 下的 profiling、scripting、gameplay、stress-test、Unity 与 coding-style 文档开始。

## 许可证

Atlas 基于 [MIT License](LICENSE) 发布。
