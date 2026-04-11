# Atlas Engine

Atlas 是一个基于 **C++20** 与 **C# (.NET 9)** 的现代分布式 MMO 游戏服务器框架，灵感来自 **BigWorld 引擎**架构，支持多进程分布式设计、负载均衡、空间分区与容错恢复，并提供 **Windows / Linux** 跨平台运行支持。

**[English](README.md)**

## 特性

- **分布式多进程架构** — LoginApp、BaseApp、CellApp、DBApp 等多进程协作，支持负载均衡与故障恢复
- **实体系统** — 实体在 Base / Cell / Client 三端分布，通过 Mailbox 进行 RPC 通信
- **C# (.NET 9) 脚本驱动** — 通过嵌入式 CoreCLR 运行 C# 脚本，`[UnmanagedCallersOnly]` 零开销互操作
- **跨平台** — 完整的 OS API 封装，Windows 和 Linux 统一构建
- **可插拔数据库** — 支持 MySQL（生产）和 XML（开发）后端
- **客户端 SDK** — 提供轻量级接入 SDK，不绑定特定客户端引擎

## 架构

```
客户端 ──► LoginApp ──► BaseAppMgr ──► BaseApp ◄──► CellApp
                                          │              │
                                        DBApp        CellAppMgr
                                          │
                                        MySQL
```

| 进程 | 职责 |
|------|------|
| **LoginApp** | 客户端认证与登录 |
| **BaseApp** | 实体状态管理、客户端代理、持久化 |
| **CellApp** | 空间分区、实体移动、AoI（感兴趣区域） |
| **DBApp** | 数据库异步读写 |
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
| [CMake](https://cmake.org/download/) | 3.20+ | 或使用 Visual Studio 内置版本 |
| [.NET 9 SDK](https://dotnet.microsoft.com/download/dotnet/9.0) | 9.0+ | C# 脚本层必须 |
| [Git](https://git-scm.com/) | 任意版本 | 版本控制与 FetchContent 依赖拉取 |

**Visual Studio 2022 安装时需选择：**
- 工作负载：**使用 C++ 的桌面开发**
- 单个组件：**用于 Windows 的 C++ CMake 工具**（可选，VS 已内置 CMake）

安装完成后验证：
```bat
cl /?                  :: MSVC 编译器——需在 VS Developer Command Prompt 中执行
cmake --version        :: 应输出 3.20+
dotnet --version       :: 应输出 9.x.x
git --version
```

#### Linux（Ubuntu 22.04+）

```bash
# 编译器与构建工具
sudo apt update
sudo apt install -y build-essential g++-12 cmake ninja-build git

# .NET 9 SDK（参考 https://learn.microsoft.com/dotnet/core/install/linux）
wget https://dot.net/v1/dotnet-install.sh
chmod +x dotnet-install.sh
./dotnet-install.sh --channel 9.0
echo 'export PATH="$HOME/.dotnet:$PATH"' >> ~/.bashrc
source ~/.bashrc

# 验证
g++ --version          # 应为 12+
cmake --version        # 应为 3.20+
dotnet --version       # 应为 9.x.x
```

> **第三方依赖**（Google Test、pugixml、RapidJSON、zlib）由 CMake `FetchContent` 在首次配置时自动下载，无需手动安装。首次配置需保证网络可用（或提前缓存好 `_deps` 目录）。

### 克隆仓库

```bash
git clone <repo-url>
cd Atlas
```

## 构建

### 配置与编译

```bash
# Windows — Visual Studio 生成器（推荐）
cmake --preset debug-windows
cmake --build --preset debug-windows

# Windows — Release
cmake --preset release-windows
cmake --build --preset release-windows

# Linux — GCC / Make
cmake --preset debug-linux
cmake --build --preset debug-linux

# 跨平台 — Ninja（需安装 Ninja）
cmake --preset debug-ninja
cmake --build --preset debug-ninja
```

> 首次配置会下载第三方依赖（约 100 MB），后续配置无需重新下载。

### CMake 选项

通过 `-D<选项>=ON/OFF` 覆盖默认值，例如：

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `ATLAS_BUILD_TESTS` | `ON` | 构建 Google Test 单元测试 |
| `ATLAS_BUILD_SERVER` | `ON` | 构建服务器进程 |
| `ATLAS_BUILD_CLIENT_SDK` | `ON` | 构建客户端 SDK |
| `ATLAS_ENABLE_ASAN` | `OFF` | 启用 AddressSanitizer（仅 GCC/Clang） |
| `ATLAS_ENABLE_TSAN` | `OFF` | 启用 ThreadSanitizer（仅 GCC/Clang） |
| `ATLAS_ENABLE_UBSAN` | `OFF` | 启用 UndefinedBehaviorSanitizer（仅 GCC/Clang） |

示例——不构建测试：
```bash
cmake --preset debug-linux -DATLAS_BUILD_TESTS=OFF
cmake --build --preset debug-linux
```

### 构建产物

```
build/debug-windows/bin/Debug/
├── machined.exe          # 机器守护进程（最先启动）
├── atlas_loginapp.exe    # 登录网关
├── atlas_baseappmgr.exe  # BaseApp 集群管理器
├── atlas_baseapp.exe     # Base 实体进程
├── atlas_cellappmgr.exe  # CellApp 集群管理器
├── atlas_dbappmgr.exe    # DBApp 集群管理器
├── atlas_dbapp.exe       # 数据库进程
├── atlas_echoapp.exe     # 最小验证进程
├── atlas_tool.exe        # 开发者命令行工具
├── atlas_engine.dll      # 核心共享库
└── zlib.dll              # 压缩运行时库
```

Linux 下：`.exe` 去掉扩展名，`.dll` 对应 `.so`。

## 测试

### 运行全部单元测试

```bash
# Windows（Visual Studio preset）
ctest --preset debug-windows

# Windows — 直接运行
ctest --test-dir build/debug-windows --build-config Debug --output-on-failure

# Linux
ctest --preset debug-ninja
# 或
ctest --test-dir build/debug-linux --output-on-failure
```

### 运行单个测试二进制

```bash
# Windows
.\build\debug-windows\bin\Debug\test_server_app.exe

# Linux
./build/debug-linux/bin/test_server_app
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

```bat
cd build\debug-windows\bin\Debug
.\atlas_echoapp.exe
```

### 完整集群启动（开发环境）

先切换到输出目录：

```bat
cd build\debug-windows\bin\Debug
```

然后在不同终端中依次执行：

```bat
# 终端 1 — 服务发现守护进程
.\machined.exe

# 终端 2 — DBApp 管理器
.\atlas_dbappmgr.exe

# 终端 3 — BaseApp 管理器
.\atlas_baseappmgr.exe

# 终端 4 — CellApp 管理器
.\atlas_cellappmgr.exe

# 终端 5 — 数据库进程（使用 XML 开发后端）
.\atlas_dbapp.exe

# 终端 6 — Base 实体进程
.\atlas_baseapp.exe

# 终端 7 — 登录网关（最后启动，开始接受客户端连接）
.\atlas_loginapp.exe
```

Linux 下将 `.\<进程名>.exe` 替换为 `./<进程名>`，目录为 `build/debug-linux/bin/`。

### 配置说明

各进程通过命令行参数和 JSON 配置文件加载配置（由 `ServerConfig` 处理）。常用参数：

| 参数 | 说明 | 示例 |
|------|------|------|
| `--config <path>` | 指定 JSON 配置文件路径 | `--config conf/baseapp.json` |
| `--machined-host <ip>` | machined 的监听地址 | `--machined-host 127.0.0.1` |
| `--port <n>` | 进程监听端口 | `--port 20010` |

### 数据库后端选择

- **XML 后端**（开发/测试）：零配置，数据存储为 XML 文件，默认启用
- **MySQL 后端**（生产）：需安装并配置 MySQL，在配置文件中指定连接信息

### C# 脚本

C# 游戏逻辑脚本位于 `scripts/` 目录，运行时通过嵌入式 CoreCLR 加载。`.NET` 运行时配置见 `runtime/atlas_server.runtimeconfig.json`。

## 目录结构

```
atlas/
├── cmake/                  CMake 工具模块
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
