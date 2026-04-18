# Atlas Engine

Atlas 是一个基于 **C++20** 与 **C# (.NET 9)** 的现代分布式 MMO 游戏服务器框架，灵感来自 **BigWorld 引擎**架构，支持多进程分布式设计、负载均衡、空间分区与容错恢复，并提供 **Windows / Linux** 跨平台运行支持。

**[English](README.md)**

## 特性

- **分布式多进程架构** — LoginApp、BaseApp、CellApp、DBApp 等多进程协作，支持负载均衡与故障恢复
- **实体系统** — 实体在 Base / Cell / Client 三端分布，通过 Mailbox 进行 RPC 通信
- **C# (.NET 9) 脚本驱动** — 通过嵌入式 CoreCLR 运行 C# 脚本，`[UnmanagedCallersOnly]` 零开销互操作
- **跨平台** — 完整的 OS API 封装，Windows 和 Linux 统一构建
- **可插拔数据库** — 支持 MySQL（生产）、SQLite（开发）和 XML（轻量回退）后端
- **客户端运行时** — C# `Atlas.Client` 骨架（实体 / 回调 / 生成器接线）已落地；完整连接 + 登录栈仍在推进（Phase 12）

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
| **CellApp** | 空间分区、实体移动、AoI（感兴趣区域） |
| **DBApp** | 基于 XML、SQLite 或 MySQL 后端的异步数据库读写 |
| **BaseAppMgr / CellAppMgr / DBAppMgr** | 各进程集群的负载均衡与协调 |
| **Reviver** | 进程崩溃检测与自动恢复 |
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
- **`WatcherRegistry`** — 基于层级路径的进程指标可观测注册表，支持通过路径字符串读写运行时数据
- **`Updatable` / `Updatables`** — 按优先级分层的每帧回调系统，迭代期间安全支持增删操作
- **`SignalDispatchTask`** — 将 OS 信号（SIGINT、SIGTERM 等）分发到事件循环中处理

## 开发环境配置

### 前置要求

#### Windows

| 工具 | 版本要求 | 说明 |
|------|----------|------|
| [Visual Studio 2022](https://visualstudio.microsoft.com/) | 17.x | 需勾选 **"使用 C++ 的桌面开发"** 工作负载 |
| [CMake](https://cmake.org/download/) | 3.28+ | 构建系统；也可使用 Visual Studio 自带的版本 |
| [.NET 9 SDK](https://dotnet.microsoft.com/download/dotnet/9.0) | 9.0+ | C# 脚本层必须；构建时自动检测 |
| [Git](https://git-scm.com/) | 任意版本 | 版本控制 |

**Visual Studio 2022 安装时需选择：**
- 工作负载：**使用 C++ 的桌面开发**

安装完成后验证：
```bat
cl /?                  :: MSVC 编译器——需在 VS Developer Command Prompt 中执行
cmake --version        :: 应为 3.28+
dotnet --version       :: 应输出 9.x.x
git --version
```

#### Linux（Ubuntu 22.04+）

```bash
# 编译器、构建工具、CMake
sudo apt update
sudo apt install -y build-essential g++-13 cmake git

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

> **第三方依赖**（Google Test、pugixml、RapidJSON、zlib、SQLite）由 CMake 通过 `FetchContent` 管理，首次配置时自动下载并缓存，无需手动安装。

> **.NET SDK 自动检测：** CMake 会通过 `DOTNET_ROOT` 环境变量或系统默认路径自动定位已安装的 .NET SDK，无需手动配置版本路径——只需安装 .NET 9+ SDK 即可。

### 克隆仓库

```bash
git clone <repo-url>
cd Atlas
```

### IDE / 编辑器配置（可选）

#### VS Code + clangd

1. 安装推荐插件（打开项目时自动提示，或手动查看 `.vscode/extensions.json`）：
   - **clangd** — 基于 compile_commands.json 的 C++ 代码智能
   - **CMake Tools** — CMake 集成
2. 复制配置模板：
   ```bash
   cp .vscode/settings.json.example .vscode/settings.json
   ```
3. CMake 在构建目录中自动生成 `compile_commands.json`（已通过 `CMAKE_EXPORT_COMPILE_COMMANDS` 配置启用）。

#### CLion

1. File -> **Open** -> 选择 Atlas 根目录（CLion 自动检测 `CMakeLists.txt`）
2. 在工具栏中选择所需的 CMake 预设（debug、release 等）
3. 无需额外配置

## 构建

### 构建命令

```bash
# 配置（默认 debug 模式）
cmake --preset debug

# 构建
cmake --build build/debug --config Debug

# 指定构建配置
cmake --preset release
cmake --build build/release --config Release

cmake --preset hybrid
cmake --build build/hybrid --config RelWithDebInfo
```

> 首次配置时通过 `FetchContent` 自动下载第三方依赖并缓存，后续构建为增量构建，仅重新编译变更的输入。

### 构建预设（通过 `CMakePresets.json`）

| 预设 | 说明 |
|------|------|
| `debug` | Debug 模式——完整调试符号，断言启用，`ATLAS_DEBUG=1` |
| `release` | 完全优化，定义 `NDEBUG` |
| `hybrid` | 优化构建 + 调试符号（等同于 RelWithDebInfo） |

### Sanitizer

| 预设 | 平台 | 说明 |
|------|------|------|
| `asan` | Linux | AddressSanitizer（GCC/Clang） |
| `asan-msvc` | Windows | AddressSanitizer（MSVC） |
| `tsan` | Linux | ThreadSanitizer |
| `ubsan` | Linux | UndefinedBehaviorSanitizer |

> TSan 和 UBSan 不支持 MSVC。

### CMake 选项

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `ATLAS_BUILD_TESTS` | `ON` | 构建单元测试和集成测试 |
| `ATLAS_BUILD_CSHARP` | `ON` | 通过 dotnet 构建 C# 项目 |
| `ATLAS_DB_MYSQL` | `OFF` | 启用 MySQL 数据库后端 |
| `ATLAS_USE_IOURING` | `OFF` | 在 Linux 上启用 io_uring |

### 构建产物

构建产物位于 `build/<预设名>/`：

```
build/debug/src/server/
├── machined/Debug/machined         # 机器守护进程（最先启动）
├── loginapp/Debug/atlas_loginapp   # 登录网关
├── baseappmgr/Debug/atlas_baseappmgr
├── baseapp/Debug/atlas_baseapp
├── dbapp/Debug/atlas_dbapp
└── EchoApp/Debug/atlas_echoapp    # 最小验证进程
```

Windows 下二进制文件带 `.exe` 扩展名。Linux 下（单配置生成器）无 `Debug/` 子目录。

## 测试

### 运行全部单元测试

```bash
cd build/debug

# 全部测试
ctest --build-config Debug

# 仅单元测试
ctest --build-config Debug --label-regex unit

# 仅集成测试
ctest --build-config Debug --label-regex integration

# 全部测试（失败时显示详细输出）
ctest --build-config Debug --output-on-failure
```

### 运行单个测试

```bash
# 按名称运行特定测试
ctest --build-config Debug -R test_math

# 详细输出
ctest --build-config Debug -R test_server_app --output-on-failure
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

## 运行

Atlas 采用多进程架构，各进程需**按顺序**独立启动。

### 进程启动顺序

```
machined → DBAppMgr → BaseAppMgr → CellAppMgr → DBApp → BaseApp → CellApp → LoginApp
```

> **注意**：`machined` 是所有进程的服务发现中心，必须最先启动。

### 快速启动（以 EchoApp 验证构建为例）

`EchoApp` 是最小化的独立验证进程，不依赖其他服务，适合快速验证构建结果：

```bash
./build/debug/src/server/EchoApp/Debug/atlas_echoapp
```

### 完整集群启动（开发环境）

在不同终端中依次执行：

```bash
# 终端 1 — 服务发现守护进程
./build/debug/src/server/machined/Debug/machined

# 终端 2 — DBApp 管理器
# ./build/debug/src/server/dbappmgr/Debug/atlas_dbappmgr  # 占位，尚未实现

# 终端 3 — BaseApp 管理器
./build/debug/src/server/baseappmgr/Debug/atlas_baseappmgr

# 终端 4 — 数据库进程（默认使用 XML 回退后端；可通过配置/CLI 切换为 SQLite）
./build/debug/src/server/dbapp/Debug/atlas_dbapp

# 终端 5 — Base 实体进程
./build/debug/src/server/baseapp/Debug/atlas_baseapp

# 终端 6 — 登录网关（最后启动，开始接受客户端连接）
./build/debug/src/server/loginapp/Debug/atlas_loginapp
```

Linux 下（单配置生成器）路径中无 `Debug/` 子目录。

### 配置说明

各进程通过命令行参数和 JSON 配置文件加载配置（由 `ServerConfig` 处理）。常用参数：

| 参数 | 说明 | 示例 |
|------|------|------|
| `--config <path>` | 指定 JSON 配置文件路径 | `--config conf/baseapp.json` |
| `--machined <host:port>` | machined 的监听地址 | `--machined 127.0.0.1:20018` |
| `--internal-port <n>` | 进程内部监听端口 | `--internal-port 20010` |
| `--external-port <n>` | 对外/客户端监听端口（如适用） | `--external-port 20013` |

### 数据库后端选择

- **XML 后端**（开发/测试）：零配置、文件式存储，仍是默认回退选项
- **SQLite 后端**（开发）：单文件关系型后端，`checkout` / `lookup` 语义更接近 MySQL；需要系统提供 SQLite 运行时库，并支持 `sqlite_path`、`sqlite_wal`、`sqlite_busy_timeout_ms` 配置
- **MySQL 后端**（生产候选）：需安装并配置 MySQL，在配置文件中指定连接信息

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

游戏逻辑以 C# 库形式组织在 `src/csharp/` 下（`Atlas.Shared`、`Atlas.Runtime`），运行时通过嵌入式 CoreCLR 加载。`.NET` 运行时配置见 `runtime/atlas_server.runtimeconfig.json`。

## 目录结构

```
atlas/
├── CMakeLists.txt          根构建文件
├── CMakePresets.json        构建预设（debug、release、sanitizer）
├── cmake/                  CMake 模块
│   ├── AtlasCompilerOptions.cmake
│   ├── Dependencies.cmake       第三方依赖（FetchContent）
│   ├── FindDotNet.cmake          .NET SDK 自动检测
│   └── AtlasDotNetBuild.cmake    C# 项目构建辅助
├── docs/
│   ├── roadmap/            各阶段开发规划文档
│   └── scripting/          C# 脚本层设计文档
├── runtime/                .NET 运行时配置
├── src/
│   ├── lib/                核心库
│   │   ├── platform/         OS 抽象层（I/O、线程、信号、文件系统）
│   │   ├── foundation/       基础设施（日志、内存、容器、时间）
│   │   ├── network/          网络通信（Socket、EventDispatcher、Channel、消息）
│   │   ├── serialization/    序列化（二进制流、XML/JSON 解析）
│   │   ├── math/             数学库（向量、矩阵、四元数）
│   │   ├── physics/          物理/碰撞（占位）
│   │   ├── resmgr/           资源管理器（存根）
│   │   ├── coro/             C++20 协程辅助（RPC await、取消传播）
│   │   ├── script/           脚本抽象层（ScriptEngine / ScriptValue）
│   │   ├── clrscript/        .NET 9 CoreCLR 嵌入（ClrHost）
│   │   ├── entitydef/        实体类型定义、数据类型、Mailbox
│   │   ├── connection/       客户端-服务器协议定义
│   │   ├── db/               数据库抽象（IDatabase + DatabaseFactory）
│   │   ├── db_mysql/         MySQL 后端（占位）
│   │   ├── db_sqlite/        SQLite 后端
│   │   ├── db_xml/           XML 后端
│   │   └── server/           服务器框架基类
│   ├── server/             服务器进程
│   │   ├── machined/         机器守护进程
│   │   ├── loginapp/         登录网关
│   │   ├── baseapp/          Base 实体宿主
│   │   ├── baseappmgr/       BaseApp 集群管理器
│   │   ├── cellapp/          空间模拟进程（占位）
│   │   ├── cellappmgr/       CellApp 集群管理器（占位）
│   │   ├── dbapp/            数据库进程
│   │   ├── dbappmgr/         DBApp 集群管理器（占位）
│   │   ├── reviver/          崩溃检测与自动恢复（占位）
│   │   └── EchoApp/          最小验证进程
│   ├── csharp/             C# 托管库
│   │   ├── Atlas.Shared/              协议类型、实体定义、RPC 契约
│   │   ├── Atlas.Runtime/             服务器侧 CoreCLR 宿主与引擎绑定
│   │   ├── Atlas.Client/              客户端实体运行时（骨架）
│   │   ├── Atlas.Generators.Def/      基于 .def 的 Source Generator（实体类 / delta 同步）
│   │   └── Atlas.Generators.Events/   事件绑定 Source Generator
│   ├── client/             控制台客户端应用（连接 + native provider）
│   └── tools/              运维工具（atlas_tool、login_stress）
└── tests/
    ├── unit/               C++ 单元测试（Google Test）
    ├── integration/        端到端集成测试（Google Test）
    └── csharp/             C# 单元与冒烟测试（Atlas.Runtime.Tests、Atlas.Generators.Tests、Atlas.RuntimeTest、Atlas.SmokeTest）
```

## 许可证

本项目基于 [MIT](LICENSE) 协议开源。
