# Atlas Engine

Atlas 是一个基于 **C++20** 与 **Python 3** 的现代分布式 MMO 游戏服务器框架，灵感来自 **BigWorld 引擎**架构，支持多进程分布式设计、负载均衡、空间分区与容错恢复，并提供 **Windows / Linux** 跨平台运行支持。

**[English](README.md)**

## 特性

- **分布式多进程架构** — LoginApp、BaseApp、CellApp、DBApp 等多进程协作，支持负载均衡与故障恢复
- **实体系统** — 实体在 Base / Cell / Client 三端分布，通过 Mailbox 进行 RPC 通信
- **Python 3 脚本驱动** — 前后端统一使用 Python 进行游戏逻辑开发
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

## 构建

### 环境要求

- CMake 3.20+
- C++20 编译器：MSVC 2022+ / GCC 10+ / Clang 12+
- Python 3.10+（脚本层，后续阶段启用）

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
├── src/
│   ├── lib/                核心库
│   │   ├── platform/         OS 抽象层
│   │   ├── foundation/       基础设施（日志、内存、容器、时间）
│   │   ├── network/          网络通信
│   │   ├── serialization/    序列化
│   │   ├── script/           脚本抽象层
│   │   ├── pyscript/         Python 3 集成
│   │   ├── entitydef/        实体定义系统
│   │   ├── connection/       通信协议
│   │   ├── db/               数据库抽象
│   │   ├── server/           服务器框架基类
│   │   └── ...
│   ├── server/             服务器进程
│   │   ├── loginapp/
│   │   ├── baseapp/
│   │   ├── cellapp/
│   │   ├── dbapp/
│   │   └── ...
│   └── client_sdk/         客户端接入 SDK
├── tests/                  测试
├── scripts/                Python 游戏脚本
├── tools/                  运维工具（Python）
└── docker/                 容器化部署
```

## 许可证

本项目基于 [MIT](LICENSE) 协议开源。
