# Atlas Engine

Atlas 是一个基于 **C++20** 与 **C# (.NET 9)** 的现代分布式 MMO 游戏服务器框架，灵感来自 **BigWorld 引擎**架构，支持多进程分布式设计、负载均衡、空间分区与容错恢复，并提供 **Windows / Linux** 跨平台运行支持。

**[English](README.md)**

## 特性

- **分布式多进程架构** — LoginApp、BaseApp、CellApp、DBApp 等多进程协作，支持负载均衡与故障恢复
- **实体系统** — 实体在 Base / Cell / Client 三端分布，通过 Mailbox 进行 RPC 通信
- **C# (.NET 9) 脚本驱动** — 通过嵌入式 CoreCLR 运行 C# 脚本，`[UnmanagedCallersOnly]` 零开销互操作
- **跨平台** — 完整的 OS API 封装，Windows 和 Linux 统一构建
- **可插拔数据库** — 支持 MySQL（生产）、SQLite（开发）和 XML（轻量回退）后端
- **客户端 SDK** — 提供轻量级接入 SDK，不绑定特定客户端引擎

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
| [Bazelisk](https://github.com/bazelbuild/bazelisk) | 最新版 | Bazel 版本管理器，读取 `.bazelversion` 自动下载对应版本的 Bazel |
| [.NET 9 SDK](https://dotnet.microsoft.com/download/dotnet/9.0) | 9.0+ | C# 脚本层必须；构建时自动检测 |
| [Git](https://git-scm.com/) | 任意版本 | 版本控制 |

**安装 Bazelisk（Windows）：**
```bat
winget install Google.Bazelisk
```

**Visual Studio 2022 安装时需选择：**
- 工作负载：**使用 C++ 的桌面开发**

安装完成后验证：
```bat
cl /?                  :: MSVC 编译器——需在 VS Developer Command Prompt 中执行
bazel --version        :: 应显示 7.x（由 Bazelisk 自动下载）
dotnet --version       :: 应输出 9.x.x
git --version
```

#### Linux（Ubuntu 22.04+）

```bash
# 编译器与构建工具
sudo apt update
sudo apt install -y build-essential g++-12 git

# Bazelisk — 自动管理 Bazel 版本
curl -Lo /usr/local/bin/bazel \
  https://github.com/bazelbuild/bazelisk/releases/latest/download/bazelisk-linux-amd64
chmod +x /usr/local/bin/bazel

# .NET 9 SDK（参考 https://learn.microsoft.com/dotnet/core/install/linux）
wget https://dot.net/v1/dotnet-install.sh
chmod +x dotnet-install.sh
./dotnet-install.sh --channel 9.0
echo 'export PATH="$HOME/.dotnet:$PATH"' >> ~/.bashrc
source ~/.bashrc

# 验证
g++ --version          # 应为 12+
bazel --version        # 应显示 7.x（由 Bazelisk 自动下载）
dotnet --version       # 应为 9.x.x
```

> **第三方依赖**（Google Test、pugixml、RapidJSON、zlib、SQLite）由 Bazel 通过 `MODULE.bazel` 和 `deps.bzl` 管理，首次构建时自动下载并缓存，无需手动安装。

> **.NET SDK 自动检测：** Bazel 会通过 `DOTNET_ROOT` 环境变量或系统默认路径自动定位已安装的 .NET SDK，无需手动配置版本路径——只需安装 .NET 9+ SDK 即可。

> **SQLite 后端运行时说明：** SQLite 后端会在运行时动态加载系统 SQLite 库（Windows 上优先 `sqlite3.dll` / `winsqlite3.dll`，Linux/macOS 上为 `libsqlite3.so*` / `libsqlite3.dylib`）。Atlas 不会把 SQLite 静态打包进构建产物。

### 克隆仓库

```bash
git clone <repo-url>
cd Atlas
```

## 构建

### 构建命令

```bash
# 构建全部（默认 debug 模式）
bazel build //...

# 指定构建配置
bazel build //... --config=debug
bazel build //... --config=release
bazel build //... --config=hybrid     # 带调试符号的优化构建

# 构建特定目标
bazel build //src/server/machined:machined
bazel build //src/lib/network:atlas_network
```

> 首次构建时自动下载第三方依赖并缓存，后续构建为增量构建，仅重新编译变更的输入。

### 构建配置（通过 `.bazelrc`）

| 配置 | 说明 |
|------|------|
| *（默认）* | Debug 模式——完整调试符号，断言启用 |
| `--config=debug` | 显式 debug 模式，`ATLAS_DEBUG=1` |
| `--config=release` | 完全优化，定义 `NDEBUG` |
| `--config=hybrid` | 优化构建 + 调试符号（等同于 RelWithDebInfo） |

### Sanitizer

| 配置 | 平台 | 说明 |
|------|------|------|
| `--config=asan` | Linux | AddressSanitizer（GCC/Clang） |
| `--config=asan-msvc` | Windows | AddressSanitizer（MSVC） |
| `--config=tsan` | Linux | ThreadSanitizer |
| `--config=ubsan` | Linux | UndefinedBehaviorSanitizer |

> TSan 和 UBSan 不支持 MSVC。

### 可选构建标志

| 标志 | 默认值 | 说明 |
|------|--------|------|
| `--define=ATLAS_DB_MYSQL=1` | `0` | 启用 MySQL 数据库后端 |
| `--define=ATLAS_USE_IOURING=1` | `0` | 在 Linux 上启用 io_uring |

### 构建产物

构建产物位于 `bazel-bin/`（由 Bazel 创建的符号链接）：

```
bazel-bin/src/server/
├── machined/machined         # 机器守护进程（最先启动）
├── loginapp/atlas_loginapp   # 登录网关
├── baseappmgr/atlas_baseappmgr
├── baseapp/atlas_baseapp
├── cellappmgr/atlas_cellappmgr
├── dbappmgr/atlas_dbappmgr
├── dbapp/atlas_dbapp
└── EchoApp/atlas_echoapp    # 最小验证进程
```

Windows 下二进制文件带 `.exe` 扩展名。

## 测试

### 运行全部单元测试

```bash
# 全部单元测试
bazel test //tests/unit:all

# 全部单元测试（详细输出）
bazel test //tests/unit:all --test_output=all

# 集成测试
bazel test //tests/integration:all
```

### 运行单个测试

```bash
# 运行特定测试目标
bazel test //tests/unit:test_math

# 详细输出
bazel test //tests/unit:test_server_app --test_output=all
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
bazel run //src/server/EchoApp:atlas_echoapp
```

### 完整集群启动（开发环境）

在不同终端中依次执行：

```bash
# 终端 1 — 服务发现守护进程
bazel run //src/server/machined:machined

# 终端 2 — DBApp 管理器
bazel run //src/server/dbappmgr:atlas_dbappmgr

# 终端 3 — BaseApp 管理器
bazel run //src/server/baseappmgr:atlas_baseappmgr

# 终端 4 — CellApp 管理器
bazel run //src/server/cellappmgr:atlas_cellappmgr

# 终端 5 — 数据库进程（默认使用 XML 回退后端；可通过配置/CLI 切换为 SQLite）
bazel run //src/server/dbapp:atlas_dbapp

# 终端 6 — Base 实体进程
bazel run //src/server/baseapp:atlas_baseapp

# 终端 7 — 登录网关（最后启动，开始接受客户端连接）
bazel run //src/server/loginapp:atlas_loginapp
```

也可以先构建后直接从 `bazel-bin/` 运行二进制文件。

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

C# 游戏逻辑脚本位于 `scripts/` 目录，运行时通过嵌入式 CoreCLR 加载。`.NET` 运行时配置见 `runtime/atlas_server.runtimeconfig.json`。

## 目录结构

```
atlas/
├── BUILD.bazel             根构建文件与功能开关配置
├── MODULE.bazel            Bazel 模块与依赖声明
├── .bazelrc                编译器选项与构建配置
├── deps.bzl                非注册中心的第三方依赖
├── docs/
│   ├── roadmap/            各阶段开发规划文档
│   └── scripting/          C# 脚本层设计文档
├── runtime/                .NET 运行时配置
├── scripts/                C# 游戏逻辑脚本（运行时加载）
│   ├── base/               Base 实体脚本
│   ├── cell/               Cell 实体脚本
│   └── common/             公共定义
├── src/
│   ├── lib/                核心库
│   │   ├── platform/         OS 抽象层（I/O、线程、信号、文件系统）
│   │   ├── foundation/       基础设施（日志、内存、容器、时间）
│   │   ├── network/          网络通信（Socket、EventDispatcher、Channel、消息）
│   │   ├── serialization/    序列化（二进制流、XML/JSON 解析）
│   │   ├── math/             数学库（向量、矩阵、四元数）
│   │   ├── physics/          物理/碰撞（存根）
│   │   ├── chunk/            世界分块/流式加载（存根）
│   │   ├── resmgr/           资源管理器（存根）
│   │   ├── script/           脚本抽象层（ScriptEngine / ScriptValue）
│   │   ├── clrscript/        .NET 9 CoreCLR 嵌入（ClrHost）
│   │   ├── entitydef/        实体类型定义、数据类型、Mailbox
│   │   ├── connection/       客户端-服务器协议定义
│   │   ├── db/               数据库抽象（IDatabase + DatabaseFactory）
│   │   ├── db_mysql/         MySQL 后端
│   │   ├── db_sqlite/        SQLite 后端（运行时加载 sqlite3）
│   │   ├── db_xml/           XML 后端
│   │   └── server/           服务器框架基类
│   ├── server/             服务器进程
│   │   ├── machined/         机器守护进程
│   │   ├── loginapp/         登录网关
│   │   ├── baseapp/          Base 实体宿主
│   │   ├── baseappmgr/       BaseApp 集群管理器
│   │   ├── cellapp/          空间模拟进程
│   │   ├── cellappmgr/       CellApp 集群管理器
│   │   ├── dbapp/            数据库进程
│   │   ├── dbappmgr/         DBApp 集群管理器
│   │   ├── reviver/          崩溃检测与自动恢复
│   │   └── EchoApp/          最小验证进程
│   ├── csharp/             C# 托管库
│   │   ├── Atlas.Shared/       协议类型、实体定义、RPC 契约
│   │   ├── Atlas.Runtime/      CoreCLR 宿主与引擎绑定
│   │   ├── Atlas.Generators.Entity/   实体类 Source Generator
│   │   ├── Atlas.Generators.Events/   事件绑定 Source Generator
│   │   └── Atlas.Generators.Rpc/      RPC 桩代码 Source Generator
│   ├── client_sdk/         客户端接入 SDK
│   └── tools/              开发者命令行工具
│       └── atlas_tool/
├── tests/
│   ├── unit/               C++ 单元测试（Google Test，60+ 个测试文件）
│   ├── integration/        集成测试存根
│   └── csharp/             C# 冒烟测试
├── tools/                  运维工具（cluster_control、db_tools、monitoring）
└── docker/                 容器化部署
```

## 许可证

本项目基于 [MIT](LICENSE) 协议开源。
