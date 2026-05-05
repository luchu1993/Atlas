# Phase 5: 服务器框架基类 (`src/lib/server/`)

**Status:** ✅ 已完成。所有子系统已实现并通过单元测试，是 `machined`、
`DBApp`、`BaseApp`、`CellApp`、`LoginApp` 等所有服务器进程的共同运行时基线。
**BigWorld 参考:** `lib/server/server_app.hpp`, `lib/server/script_app.hpp`,
`lib/server/entity_app.hpp`

## 目标

为所有 Atlas 服务器进程提供统一运行时骨架。每个进程继承适当层级的基类，
注册消息处理器后即获得：事件循环、网络通信、C# 脚本集成、定时器、
后台任务、信号处理、运行时监控。

## 类层次

```
ServerApp                — 事件循环、GameClock、信号、Updatable、Watcher、配置
  ├── ManagerApp         — 管理进程 (BaseAppMgr / CellAppMgr / DBAppMgr / Reviver)
  └── ScriptApp          — + ClrScriptEngine、INativeApiProvider、脚本生命周期
        └── EntityApp    — + BgTaskManager、EntityDefRegistry、脚本定时器
              ├── BaseApp
              └── CellApp
```

## 关键设计决策

- **EventDispatcher / NetworkInterface 所有权**：由 `main()` 创建后注入
  `ServerApp` 构造函数，便于测试注入 mock。
- **Tick 驱动**：`AdvanceTime()` 注册为 `dispatcher.AddRepeatingTimer`，
  统一由 `EventDispatcher::Run()` 驱动。固定步长 `1s / update_hertz`。
- **脚本定时器**：C# 在 `Atlas.Runtime` 中自管理，经 `ScriptEngine::OnTick(dt)`
  触发；C++ 不提供定时器注册 API。
- **消息 ID**：全局唯一 `uint16_t`，按进程类型划分范围（见下表）。C# RPC
  通过转发消息承载，不占用独立 MessageID。
- **`INativeApiProvider` 生命周期**：`ScriptApp::Init()` 中创建并注册到全局；
  `Fini()` 中清理。**约束：** `CreateNativeProvider()` 不可依赖 CLR
  （此时尚未启动）。
- **`WatcherRegistry` 所有权**：`ServerApp` 成员，非全局单例（利于单元
  测试与多实例运行）。
- **`NetworkInterface` 端口绑定时序**：构造时仅建 socket，`Init()` 中根据
  `ServerConfig` 绑定端口。
- **子类配置扩展**：`ServerAppOption<T>` 模板静态实例，`ServerConfig::Load()`
  统一从 JSON 加载，`RegisterWatchers()` 统一注册 Watcher。

## 消息 ID 分配规范

| 范围 | 用途 |
|---|---|
| 0 – 99 | 保留 |
| 100 – 199 | 公共消息 (Heartbeat, Shutdown, …) |
| 1000 – 1099 | machined |
| 2000 – 2999 | BaseApp 内部接口 |
| 3000 – 3999 | CellApp 内部接口 |
| 4000 – 4999 | DBApp 内部接口 |
| 5000 – 5999 | LoginApp 接口 |
| 6000 – 6999 | BaseAppMgr 接口 |
| 7000 – 7999 | CellAppMgr 接口 |
| 8000 – 8999 | DBAppMgr 接口 |
| 10000 – 19999 | 外部接口（客户端 ↔ 服务器） |
| 50000 – 59999 | C# RPC 转发 |
