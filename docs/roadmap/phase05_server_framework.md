# Phase 5: 服务器框架基类 (`src/lib/server/`)

> 前置依赖: 脚本层运行时基线就绪（Script Phase 0-3 + Script Phase 4 最小子集）
> BigWorld 参考: `lib/server/server_app.hpp`, `lib/server/script_app.hpp`, `lib/server/entity_app.hpp`

---

## 目标

为所有 Atlas 服务器进程提供统一的运行时骨架。每个进程继承适当层级的基类，注册消息处理器，即可获得：事件循环、网络通信、C# 脚本集成、定时器、后台任务、信号处理、运行时监控。

## 验收状态（2026-04-18）

✅ **Phase 5 已全部完成。** 所有子系统均已实现并通过单元测试，是当前 `machined`、`DBApp`、`BaseApp`、`LoginApp`、`EchoApp` 等进程的共同运行时基线。

## 验收标准

- [x] `ServerApp` 基类可启动主循环，处理信号，优雅关闭 ✅
- [x] `ServerApp` 的 tick 系统可工作：`on_start_of_tick` → `Updatable::update()` → `on_tick_complete` ✅
- [x] `ServerApp` tick 性能监控可工作：慢帧检测 + Watcher 统计 ✅
- [x] `ScriptApp` 可初始化 `ClrScriptEngine` 并在 tick 中驱动 C# 脚本 ✅
- [x] `EntityApp` 提供 `BgTaskManager` 和脚本定时器 ✅
- [x] `ManagerApp` 为无脚本的管理进程提供轻量基类 ✅
- [x] `ServerConfig` 可从命令行和 JSON 文件加载配置 ✅
- [x] `ServerAppOption<T>` 可声明子类配置项，自动从 JSON 加载并注册 Watcher ✅
- [x] Watcher 系统可注册和查询运行时指标 ✅
- [x] 至少一个最小示例进程（EchoApp）可基于框架启动并运行 ✅
- [x] 全部新增代码有单元测试 ✅

---

## 1. BigWorld 架构分析与 Atlas 适配 ✅ 已完成

已完成 BigWorld → Atlas 的类层次映射，并以 C# (CoreCLR) 替代 Python 作为脚本宿主。

**Atlas 类层次（已实现）:**

```
ServerApp                     — 事件循环, GameClock, 信号, Updatable, Watcher, 配置
  ├── ManagerApp              — 管理进程 (BaseAppMgr, CellAppMgr, DBAppMgr, Reviver)
  └── ScriptApp               — + ClrScriptEngine, INativeApiProvider, 脚本生命周期
        └── EntityApp          — + BgTaskManager, EntityDefRegistry, 脚本定时器
              ├── BaseApp
              └── CellApp
```

---

## 2. 实现步骤

### Step 5.1: ServerConfig — 配置加载 ✅ 已完成

`server_config.{h,cc}` + `server_app_option.h` 均已实现。支持命令行参数 (`--type`, `--name`, `--machined`, `--internal-port`, `--external-port`, `--config`, `--log-level`) 与 JSON 配置文件加载，命令行覆盖 JSON，并通过 `ServerAppOption<T>` 模板统一声明子类配置项 + 自动注册 Watcher。单元测试见 `tests/unit/test_server_config.cpp`（353 行）。

---

### Step 5.2: Updatable — tick 回调注册系统 ✅ 已完成

`updatable.{h,cc}` 已实现分层级 Updatable 系统，支持 `update()` 过程中安全注册/注销。单元测试见 `tests/unit/test_updatable.cpp`（258 行）。

---

### Step 5.3: ServerApp — 核心基类 ✅ 已完成

`server_app.{h,cc}` 已实现完整的 `run_app()` 生命周期：

- `init() → run() → fini()` 三阶段流程
- `advance_time()` 对齐 BigWorld 6 步（含 `on_end_of_tick` / `on_start_of_tick` / `on_tick_complete` 钩子顺序）
- 固定步长 tick（`1s / update_hertz`）驱动 `GameClock`
- 信号处理通过 `SignalDispatchTask`（FrequentTask 适配器）集成到 `EventDispatcher`
- Tick 性能监控：慢帧检测 + Watcher 统计（`tick/duration_ms`, `tick/max_duration_ms`, `tick/slow_count`）
- 文件描述符/句柄限制提升（`raise_fd_limit()`，Linux `setrlimit` / Windows `_setmaxstdio`）
- `NetworkInterface::set_extension_data(this)` 供消息处理器回溯 app 实例

单元测试见 `tests/unit/test_server_app.cpp`（272 行）。

---

### Step 5.4: ScriptApp — C# 脚本集成层 ✅ 已完成

`script_app.{h,cc}` 已实现：

- `init()` 中创建 `INativeApiProvider` → 初始化 `ClrScriptEngine` → 加载 C# 程序集 → 触发 `on_init(false)` → `on_script_ready()` 钩子
- `on_tick_complete()` 中驱动 C# `on_tick(dt)`
- `fini()` 中触发 `on_shutdown()` 并清理 CLR
- `reload_scripts()` 存根已就位，待 Script Phase 5 热重载机制完善

单元测试见 `tests/unit/test_script_app.cpp`（298 行）。

---

### Step 5.5: EntityApp — 实体进程基类 ✅ 已完成

`entity_app.{h,cc}` 已实现：

- `BgTaskManager` 后台任务管理器
- `EntityDefRegistry` 接口（Phase 8 完善填充）
- SIGQUIT 信号处理 → 打印调用栈（诊断无响应进程）
- C# 侧定时器由 `Atlas.Runtime` 自管理，经 `ScriptEngine::on_tick()` 驱动（C++ 侧不需要 ScriptTimers 对等物）

单元测试见 `tests/unit/test_entity_app.cpp`（182 行）。

---

### Step 5.6: ManagerApp — 管理进程基类 ✅ 已完成

`manager_app.{h,cc}` 已实现轻量无脚本基类，供 `BaseAppMgr` / `CellAppMgr` / `DBAppMgr` / `Reviver` 使用。

---

### Step 5.7: Watcher — 运行时监控 ✅ 已完成

`watcher.{h,cc}` 已实现：

- `WatcherEntry` 基类 + `DataWatcher<T>` / `FunctionWatcher<T>` / `MemberWatcher<T, Obj>` 派生
- 层级路径 (如 `app/uptime_seconds`, `tick/duration_ms`, `network/bytes_sent`)
- ReadOnly / ReadWrite 模式
- `list()` 子树枚举，`snapshot()` 全量导出
- `WatcherRegistry` 归 `ServerApp` 所有（非全局单例，利于单元测试）
- `ServerApp::register_watchers()` 默认注册 `app/*` 与 `tick/*` 指标，并遍历 `ServerAppOption` 统一注册

单元测试见 `tests/unit/test_watcher.cpp`（277 行）+ `test_watcher_forwarder.cpp`。

远程查询协议集成见 Phase 6 (machined)。

---

### Step 5.8: 进程间消息接口约定 ✅ 已完成

`common_messages.h` 已实现 `Heartbeat`、`ShutdownRequest` 等公共消息。Atlas 使用全局唯一 `MessageID` (uint16_t)，按范围分配给不同进程类型（见文档内消息 ID 分配规范）。C# RPC 通过少量"RPC 转发"消息承载，不占用独立 MessageID。

**消息 ID 分配规范:**

| 范围 | 用途 |
|------|------|
| 0 – 99 | 保留 (未来) |
| 100 – 199 | 公共消息 (Heartbeat, Shutdown, etc.) |
| 1000 – 1099 | machined 消息 |
| 2000 – 2999 | BaseApp 内部接口 |
| 3000 – 3999 | CellApp 内部接口 |
| 4000 – 4999 | DBApp 内部接口 |
| 5000 – 5999 | LoginApp 接口 |
| 6000 – 6999 | BaseAppMgr 接口 |
| 7000 – 7999 | CellAppMgr 接口 |
| 8000 – 8999 | DBAppMgr 接口 |
| 10000 – 19999 | 外部接口 (客户端 ↔ 服务器) |
| 50000 – 59999 | C# RPC 转发 |

---

### Step 5.9: CMakeLists.txt 构建配置 ✅ 已完成

`src/lib/server/CMakeLists.txt` 已就位，包含全部源文件 + 依赖 (`atlas_foundation`, `atlas_network`, `atlas_platform`, `atlas_serialization`, `atlas_script`, `atlas_clrscript`)。

---

### Step 5.10: 最小示例进程 EchoApp ✅ 已完成

`src/server/EchoApp/{echo_app.h, echo_app.cc, main.cc, CMakeLists.txt}` 均已就位，继承自 `ManagerApp`，端到端验证了框架的可用性（进程启动 → tick → 信号 → 优雅关闭 → Watcher 查询 → 配置加载）。

---

## 3. 文件清单汇总（现状）

```
src/lib/server/
├── CMakeLists.txt
├── common_messages.h
├── entity_app.h / .cc
├── entity_types.h
├── ipv4_networks.h / .cc
├── machined_client.h / .cc
├── manager_app.h / .cc
├── script_app.h / .cc
├── server_app.h / .cc
├── server_app_option.h
├── server_config.h / .cc
├── signal_dispatch_task.h
├── updatable.h / .cc
└── watcher.h / .cc

src/server/EchoApp/
├── CMakeLists.txt
├── echo_app.h / .cc
└── main.cc

tests/unit/
├── test_server_config.cpp
├── test_updatable.cpp
├── test_server_app.cpp
├── test_script_app.cpp
├── test_entity_app.cpp
├── test_watcher.cpp
└── test_watcher_forwarder.cpp
```

---

## 4. 关键设计决策（保留记录）

以下决策已在实现中落地，作为后续维护参考：

- **EventDispatcher / NetworkInterface 所有权**：由 `main()` 创建后注入 `ServerApp` 构造函数（便于测试注入 mock）。
- **tick 驱动方式**：`advance_time()` 注册为 `dispatcher.add_repeating_timer(tick_interval, ...)`，由 `EventDispatcher::run()` 统一驱动。
- **脚本定时器**：C# 侧在 `Atlas.Runtime` 中自管理，经 `ScriptEngine::on_tick(dt)` 触发；C++ 侧不提供定时器注册 API。
- **消息 ID**：全局唯一 `uint16_t`，按进程类型划分范围；C# RPC 通过转发消息承载而非每方法独占 ID。
- **`INativeApiProvider` 生命周期**：`ScriptApp::init()` 中创建 → 注册到全局 → 启动 CLR；`fini()` 中清理。**约束:** `create_native_provider()` 不可依赖 CLR（彼时尚未启动）。
- **`WatcherRegistry` 所有权**：`ServerApp` 成员，非全局单例（利于单元测试与多实例运行）。
- **`NetworkInterface` 端口绑定时序**：构造时仅建 socket，`init()` 中根据 `ServerConfig` 绑定端口。
- **子类配置扩展**：`ServerAppOption<T>` 模板静态实例，`ServerConfig::load()` 中统一从 JSON 加载，`register_watchers()` 中统一注册 Watcher。
