# Atlas Engine

Atlas 是一个基于 **C++20** 与 **C# (.NET 9)** 的现代分布式 MMO 游戏服务器框架，灵感来自 **BigWorld 引擎**架构，支持多进程分布式设计、负载均衡、空间分区与容错恢复，并提供 **Windows / Linux** 跨平台运行支持。

**[English](README.md)**

## 特性

- **分布式多进程架构** — LoginApp、BaseApp、CellApp、DBApp 及其 Manager 多进程协作，支持负载均衡与故障恢复
- **空间分区** — CellApp + CellAppMgr 维护 BSP 树分区，支持基于 Witness 的 AoI、Ghost 实体与跨 Cell offload
- **实体系统** — 实体在 Base / Cell / Client 三端分布，通过 Mailbox 进行 RPC 通信
- **C# (.NET 9) 脚本驱动** — 通过嵌入式 CoreCLR 运行 C# 脚本，`[UnmanagedCallersOnly]` 零开销互操作
- **跨平台** — 完整的 OS API 封装，Windows 和 Linux 统一构建
- **可插拔数据库** — 支持 MySQL（生产）、SQLite（开发）和 XML（轻量回退）后端
- **客户端运行时** — C# `Atlas.Client` 已落地桌面与 Unity 两套适配；`Atlas.Generators.Def` Source Generator 从 `.def` 自动生成实体类 / delta 同步代码

## 架构

```
客户端 ──► LoginApp ──► BaseAppMgr ──► BaseApp ◄──► CellApp
                                          │              │
                                        DBApp        CellAppMgr
                                          │
                                   MySQL / SQLite / XML
```

| 进程 | 职责 |
|------|------|
| **LoginApp** | 客户端认证与登录 |
| **BaseApp** | 实体状态管理、客户端代理、持久化 |
| **CellApp** | 空间模拟、实体移动、AoI（感兴趣区域）、Ghost 复制 |
| **CellAppMgr** | BSP 树空间分区、Cell offload、几何分发 |
| **DBApp** | 基于 XML、SQLite 或 MySQL 后端的异步数据库读写 |
| **BaseAppMgr / DBAppMgr** | 集群负载均衡与协调 |
| **Reviver** | 进程崩溃检测与自动恢复（占位） |
| **machined** | 机器守护进程，服务注册与发现 |

## 服务器框架（`src/lib/server/`）

`server` 库提供所有 Atlas 服务器进程共用的基类层级：

```
ServerApp
├── ManagerApp          — 管理/守护进程（无脚本引擎）
│   ├── BaseAppMgr
│   ├── CellAppMgr
│   ├── DBAppMgr
│   ├── machined
│   └── EchoApp         — 最小验证进程
└── ScriptApp           — ServerApp + CoreCLR 脚本层
    └── EntityApp       — ScriptApp + 实体定义 + 后台任务线程池
        ├── BaseApp
        └── CellApp
```

`ServerApp` 提供的核心组件：

- **`ServerConfig`** — 从命令行参数和 JSON 配置文件加载进程配置
- **`MachinedClient`** — 与 machined 的 TCP 连接，用于注册、心跳、服务发现和 Birth/Death 通知
- **`WatcherRegistry`** — 基于层级路径的进程指标可观测注册表
- **`Updatable` / `Updatables`** — 按优先级分层的每帧回调系统，迭代期间安全支持增删
- **`SignalDispatchTask`** — 将 OS 信号（SIGINT、SIGTERM 等）分发到事件循环

## 开发环境配置

### 前置要求

#### Windows

| 工具 | 版本要求 | 说明 |
|------|----------|------|
| [Visual Studio 2022](https://visualstudio.microsoft.com/) | 17.x | 需勾选 **"使用 C++ 的桌面开发"** 工作负载 |
| [CMake](https://cmake.org/download/) | 3.28+ | 构建系统；也可使用 Visual Studio 自带版本 |
| [.NET 9 SDK](https://dotnet.microsoft.com/download/dotnet/9.0) | 9.0+ | C# 脚本层必需；构建时自动检测 |
| [Python](https://www.python.org/) | 3.9+ | 仅 `tools/build.{bat,sh}` 与性能分析脚本需要 |
| [Git](https://git-scm.com/) | 任意版本 | 版本控制 |
| [Ninja](https://ninja-build.org/) | 1.11+ | 可选 — 首次运行 `tools/build.bat` 时若未找到会自动下载到 `.tmp/ninja/` |

> `tools/build.bat` 内部通过 `vswhere` + `vcvars64.bat` 自动加载 MSVC 环境，**无需手动打开 x64 Native Tools Command Prompt**。

安装完成后验证：
```bat
cmake --version        :: 应为 3.28+
dotnet --version       :: 应输出 9.x.x
python --version       :: 应为 3.9+
```

#### Linux（Ubuntu 22.04+）

```bash
# 编译器、构建工具、CMake、Ninja
sudo apt update
sudo apt install -y build-essential g++-13 cmake ninja-build git python3

# .NET 9 SDK（参考 https://learn.microsoft.com/dotnet/core/install/linux）
wget https://dot.net/v1/dotnet-install.sh
chmod +x dotnet-install.sh
./dotnet-install.sh --channel 9.0
echo 'export PATH="$HOME/.dotnet:$PATH"' >> ~/.bashrc
source ~/.bashrc

# 验证
g++ --version          # 应为 13+
cmake --version        # 应为 3.28+
dotnet --version       # 应为 9.x.x
```

> **第三方依赖**（Google Test、pugixml、RapidJSON、zlib、SQLite、Tracy、mimalloc）由 CMake 通过 `FetchContent` 管理，首次配置时自动下载。

### 克隆仓库

```bash
git clone <repo-url>
cd Atlas
```

### IDE / 编辑器配置（可选）

#### VS Code + clangd

1. 安装推荐插件（首次打开项目时自动提示，或参考 `.vscode/extensions.json`）：
   - **clangd** — 基于 compile_commands.json 的 C++ 智能感知
   - **CMake Tools** — CMake 集成（Windows 下自动加载 MSVC 环境）
2. 复制配置模板：
   ```bash
   cp .vscode/settings.json.example .vscode/settings.json
   ```
3. CMake 自动生成 `compile_commands.json`（已默认开启 `CMAKE_EXPORT_COMPILE_COMMANDS`）。

#### CLion / Visual Studio

直接打开 Atlas 根目录，IDE 会自动识别 `CMakePresets.json` 并加载工具链（含 MSVC 环境）。

## 构建

### 一键脚本（推荐）

`tools/build.py` 包装了 cmake 配置 + 构建，并自动处理两个日常痛点：

- **Windows**：通过 vswhere + vcvars64.bat 自动加载 MSVC 环境，无需手动打开 x64 Native Tools Shell
- **任意 OS**：检测不到 `ninja` 时自动从官方 GitHub Release 下载对应平台二进制到 `.tmp/ninja/`

```bash
# Windows（cmd 或 PowerShell）
tools\build.bat debug
tools\build.bat profile
tools\build.bat release

# Linux / macOS
tools/build.sh debug
tools/build.sh profile
tools/build.sh release

# 选项（适用于所有 preset）
tools\build.bat debug --clean         # 配置前清空 build/<preset>
tools\build.bat debug --config-only   # 只 configure，不 build
tools\build.bat debug --build-only    # 跳过 configure，直接 build
```

### 直接调用 cmake（进阶）

```bash
# debug 预设使用 Ninja Multi-Config，需要 ninja 在 PATH 且 MSVC 环境已加载
cmake --preset debug
cmake --build build/debug --config Debug

# profile / release 仍走默认生成器（Windows 是 VS solution，Linux 是 Make）
cmake --preset profile
cmake --build build/profile --config RelWithDebInfo

cmake --preset release
cmake --build build/release --config Release
```

### 构建预设

| 预设 | 生成器 | 配置 | 说明 |
|------|--------|------|------|
| `debug` | Ninja Multi-Config | Debug | `/Z7` 调试信息 + `atlas_common.h` PCH，迭代最快 |
| `release` | 默认 | Release | 完全优化，`NDEBUG`，**不含测试** |
| `hybrid` | 默认 | RelWithDebInfo | 优化 + 调试符号 |
| `profile` | 默认 | RelWithDebInfo | Tracy + viewer + CLI 工具，**不含测试**，性能分析专用 |

> `debug` 预设切到 Ninja Multi-Config 是因为 MSBuild 的项目级开销主导了完整 Debug 迭代；`/Z7`（嵌入 CodeView）规避了并发 cl.exe 在 `/Zi` 下争抢 mspdbsrv.exe 锁的问题。

### Sanitizer

| 预设 | 平台 | 说明 |
|------|------|------|
| `asan` | Linux | AddressSanitizer（GCC/Clang） |
| `asan-msvc` | Windows | AddressSanitizer（MSVC） |
| `tsan` | Linux | ThreadSanitizer |
| `ubsan` | Linux | UndefinedBehaviorSanitizer |

> TSan 与 UBSan 不支持 MSVC。CI 在 `.github/workflows/sanitizers.yml` 中每周跑一次 `asan` + `ubsan`。

### CMake 选项

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `ATLAS_BUILD_TESTS` | `ON` | 构建单元 + 集成测试 |
| `ATLAS_BUILD_CSHARP` | `ON` | 通过 dotnet 构建 C# 项目 |
| `ATLAS_DB_MYSQL` | `OFF` | 启用 MySQL 数据库后端 |
| `ATLAS_USE_IOURING` | `OFF` | 启用 io_uring poller（仅 Linux） |
| `ATLAS_HEAP_ALLOCATOR` | `mimalloc` | `std` 或 `mimalloc` 堆后端 |

### 构建产物

无论使用哪个生成器，所有 EXE / DLL / .lib 都落到扁平的 `bin/<preset>/` 目录：

```
bin/debug/
├── machined.exe
├── atlas_loginapp.exe
├── atlas_baseappmgr.exe
├── atlas_baseapp.exe
├── atlas_cellappmgr.exe
├── atlas_cellapp.exe
├── atlas_dbapp.exe
├── atlas_echoapp.exe
├── atlas_client.exe
├── atlas_tool.exe
├── Atlas.Runtime.dll
├── Atlas.Shared.dll
├── ...                  （其他托管程序集 / 运行时 DLL）
└── test_*.exe           （仅 debug 预设包含测试 exe）
```

Linux 下省略 `.exe` 后缀。

## 测试

```bash
cd build/debug

# 全部测试
ctest --build-config Debug --output-on-failure

# 仅单元测试
ctest --build-config Debug --label-regex unit

# 仅集成测试
ctest --build-config Debug --label-regex integration

# 单个测试
ctest --build-config Debug -R test_math --output-on-failure
```

### C# 测试

```bash
dotnet test tests/csharp
```

### 代码风格检查（提交前必须通过）

```bash
# Windows（使用 VS 内置 clang-format）
"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\x64\bin\clang-format.exe" --dry-run --Werror <修改的文件>

# Linux / 跨平台
clang-format --dry-run --Werror <修改的文件>

# 自动修复
clang-format -i <修改的文件>
```

CI 在 `.github/workflows/clang-format.yml` 中强制使用 clang-format 19.1.5。

## 运行

Atlas 采用多进程架构，各进程需**按顺序**独立启动。

### 进程启动顺序

```
machined → BaseAppMgr → CellAppMgr → DBApp → BaseApp → CellApp → LoginApp
```

> **注意**：`machined` 是所有进程的服务发现中心，必须最先启动。

### 快速启动 — EchoApp（验证构建）

`EchoApp` 是无外部依赖的最小独立进程，用于验证构建：

```bash
./bin/debug/atlas_echoapp        # Windows 加 .exe 后缀
```

### 完整集群启动（开发环境）

每个进程开一个终端，按顺序启动：

```bash
./bin/debug/machined
./bin/debug/atlas_baseappmgr
./bin/debug/atlas_cellappmgr
./bin/debug/atlas_dbapp
./bin/debug/atlas_baseapp
./bin/debug/atlas_cellapp
./bin/debug/atlas_loginapp        # 最后启动 — 打开外部端口接受客户端
```

多客户端压测请使用 `tools/cluster_control/run_baseline_profile.{sh,ps1}` 与 `src/tools/world_stress/`。

### 配置说明

各进程通过命令行参数和 JSON 配置文件加载配置（由 `ServerConfig` 处理）。常用参数：

| 参数 | 说明 | 示例 |
|------|------|------|
| `--config <path>` | 指定 JSON 配置文件路径 | `--config conf/baseapp.json` |
| `--machined <host:port>` | machined 监听地址 | `--machined 127.0.0.1:20018` |
| `--internal-port <n>` | 进程内部监听端口 | `--internal-port 20010` |
| `--external-port <n>` | 对外/客户端监听端口（如适用） | `--external-port 20013` |

### 数据库后端选择

- **XML 后端**（开发/测试）：零配置、文件式存储；默认回退选项
- **SQLite 后端**（开发）：单文件关系型后端，`checkout` / `lookup` 语义接近 MySQL；支持 `sqlite_path`、`sqlite_wal`、`sqlite_busy_timeout_ms`
- **MySQL 后端**（生产候选）：需运行 MySQL 实例；构建时需 `-DATLAS_DB_MYSQL=ON`

数据库配置示例：

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

常用 DBApp CLI 覆盖参数：

```bash
--db-type sqlite
--db-sqlite-path data/atlas_dev.sqlite3
--db-sqlite-wal true
--db-sqlite-busy-timeout-ms 5000
```

### C# 脚本

游戏逻辑以 C# 库形式组织在 `src/csharp/` 下（`Atlas.Shared`、`Atlas.Runtime`），运行时通过嵌入式 CoreCLR 加载。脚本示例项目位于 `samples/base/`、`samples/client/`、`samples/stress/`。`Atlas.Generators.Def` Source Generator 基于 `.def` 文件生成实体类与 delta 同步代码。

`.NET` 运行时配置见 `runtime/atlas_server.runtimeconfig.json`。

## 性能分析

`profile` 预设启用 Tracy 仪器并自动下载 viewer + CLI 工具（`tracy-capture`、`tracy-csvexport`）到 `bin/profile/`。

```bash
# 构建 profile
tools\build.bat profile

# 跑基线（200 客户端、120 秒）— Linux / Git Bash
bash tools/cluster_control/run_baseline_profile.sh

# Windows PowerShell
.\tools\cluster_control\run_baseline_profile.ps1 -Clients 50 -DurationSec 60

# 对比两次 cellapp 捕获
python tools/profile/compare_tracy.py \
    .tmp/prof/baseline/cellapp_<old>.tracy \
    .tmp/prof/baseline/cellapp_<new>.tracy
```

捕获文件落在 `.tmp/prof/baseline/`，命名格式 `<进程>_<git短哈希>_<时间戳>.tracy`。

## 目录结构

```
atlas/
├── CMakeLists.txt              根构建文件
├── CMakePresets.json           构建预设（debug、release、profile、hybrid、sanitizers）
├── cmake/                      CMake 模块
│   ├── AtlasCompilerOptions.cmake
│   ├── AtlasOutputDirectory.cmake  扁平 bin/<preset>/ 布局
│   ├── Dependencies.cmake          第三方依赖（FetchContent）
│   ├── FindDotNet.cmake            .NET SDK 自动检测
│   └── AtlasDotNetBuild.cmake      C# 项目构建辅助
├── docs/                       设计文档（roadmap、scripting、gameplay、rpc、optimization 等）
├── runtime/                    .NET 运行时配置
├── samples/                    示例游戏脚本（base / client / stress）
├── src/
│   ├── lib/                    核心库
│   │   ├── platform/             OS 抽象层（I/O、线程、信号、文件系统）
│   │   ├── foundation/           基础设施（日志、内存、容器、时间、atlas_common.h PCH）
│   │   ├── network/              Socket、RUDP、EventDispatcher、Channel、消息
│   │   ├── serialization/        二进制流、XML/JSON 解析
│   │   ├── math/                 向量、矩阵、四元数
│   │   ├── physics/              物理 / 碰撞（占位）
│   │   ├── resmgr/               资源管理器（占位）
│   │   ├── coro/                 C++20 协程辅助（RPC await、取消传播）
│   │   ├── script/               脚本抽象（ScriptEngine / ScriptValue）
│   │   ├── clrscript/            .NET 9 CoreCLR 嵌入（ClrHost、native-API provider）
│   │   ├── entitydef/            实体类型定义、数据类型、Mailbox
│   │   ├── connection/           客户端-服务器协议定义
│   │   ├── space/                Space / Cell 共享类型（cellapp / cellappmgr 共用）
│   │   ├── db/                   数据库抽象（IDatabase + DatabaseFactory）
│   │   ├── db_mysql/             MySQL 后端
│   │   ├── db_sqlite/            SQLite 后端
│   │   ├── db_xml/               XML 后端
│   │   └── server/               服务器框架基类（ServerApp / EntityApp / ManagerApp）
│   ├── server/                 服务器进程
│   │   ├── machined/             机器守护进程
│   │   ├── loginapp/             登录网关
│   │   ├── baseappmgr/           BaseApp 集群管理器
│   │   ├── baseapp/              Base 实体宿主
│   │   ├── cellappmgr/           CellApp 集群管理器（BSP 树分区、offload）
│   │   ├── cellapp/              空间模拟（witness、AoI、ghost、controller）
│   │   ├── dbapp/                数据库进程
│   │   ├── dbappmgr/             DBApp 集群管理器（占位）
│   │   ├── reviver/              崩溃检测与恢复（占位）
│   │   └── EchoApp/              最小验证进程
│   ├── csharp/                 C# 托管库
│   │   ├── Atlas.Shared/             协议类型、实体定义、RPC 契约
│   │   ├── Atlas.Runtime/            服务器侧 CoreCLR 宿主与引擎绑定
│   │   ├── Atlas.Client/             客户端实体运行时（callbacks / factory / manager）
│   │   ├── Atlas.Client.Desktop/     桌面客户端适配
│   │   ├── Atlas.Client.Unity/       Unity asmdef + 适配
│   │   ├── Atlas.ClrHost/            CoreCLR hostfxr 包装
│   │   ├── Atlas.Generators.Def/     Source Generator：.def → 实体类 / delta 同步
│   │   └── Atlas.Tools.DefDump/      .def 检视 / dump 工具
│   ├── client/                 控制台客户端应用（连接 + native provider）
│   └── tools/                  运维工具
│       ├── atlas_tool/            多功能 CLI（配置校验、watcher 检视等）
│       ├── login_stress/          仅登录的压测驱动
│       ├── world_stress/          完整集群压测驱动（被 run_baseline_profile 调用）
│       └── crash_demo/            崩溃处理验证
├── tests/
│   ├── unit/                   C++ 单元测试（Google Test）
│   ├── integration/            端到端集成测试（Google Test）
│   └── csharp/                 C# 测试（Atlas.Runtime.Tests、Atlas.Generators.Tests、Atlas.SmokeTest）
└── tools/                      构建 / 集群 / 性能分析辅助
    ├── build.py / .bat / .sh     一键 configure + build，自动 vcvars + Ninja 下载
    ├── cluster_control/          多进程编排 + 基线 runner
    └── profile/                  Tracy 捕获对比与分析脚本
```

## 许可证

本项目基于 [MIT](LICENSE) 协议开源。
