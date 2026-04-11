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

## 构建

### 环境要求

- CMake 3.20+
- C++20 编译器：MSVC 2022+ / GCC 10+ / Clang 12+
- .NET 9 SDK（脚本层）

### 编译

```bash
# Windows (Visual Studio)
cmake --preset debug-windows
cmake --build build/debug-windows --config Debug

# 跨平台 (Ninja)
cmake --preset debug-ninja
cmake --build build/debug-ninja

# 手动指定
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug
```

### 测试

```bash
ctest --test-dir build --build-config Debug --output-on-failure
```

## 目录结构

```
atlas/
├── cmake/                  CMake 工具模块
├── runtime/                .NET 运行时配置
├── src/
│   ├── lib/                核心库
│   │   ├── platform/         OS 抽象层
│   │   ├── foundation/       基础设施（日志、内存、容器、时间）
│   │   ├── network/          网络通信
│   │   ├── serialization/    序列化
│   │   ├── script/           脚本抽象层（ScriptEngine / ScriptValue）
│   │   ├── clrscript/        .NET 9 CoreCLR 嵌入（ClrHost）
│   │   ├── entitydef/        实体定义系统
│   │   ├── connection/       通信协议
│   │   ├── db/               数据库抽象（IDatabase + DatabaseFactory）
│   │   ├── db_mysql/         MySQL 后端
│   │   ├── db_xml/           XML 后端
│   │   ├── server/           服务器框架基类
│   │   └── ...
│   ├── server/             服务器进程
│   │   ├── loginapp/
│   │   ├── baseapp/
│   │   ├── baseappmgr/
│   │   ├── dbapp/
│   │   ├── machined/
│   │   ├── EchoApp/          最小验证进程
│   │   └── ...
│   └── client_sdk/         客户端接入 SDK
├── src/tools/              开发工具
│   └── atlas_tool/
├── tests/
│   ├── unit/               C++ 单元测试（Google Test）
│   └── csharp/             C# 冒烟测试
├── tools/                  运维工具
└── docker/                 容器化部署
```

## 许可证

本项目基于 [MIT](LICENSE) 协议开源。
