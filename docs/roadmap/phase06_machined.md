# Phase 6: machined — 进程管理与服务发现

> 前置依赖: Phase 5 (服务器框架基类)
> BigWorld 参考: `server/tools/bwmachined/`, `lib/network/machine_guard.hpp`, `lib/network/machined_utils.hpp`

---

## 目标

实现集群基石服务 `machined`，以及配套的客户端库 `MachinedClient`。
每台物理/虚拟机运行一个 machined 守护进程，提供进程注册、服务发现、
存活检测和事件通知。所有服务器进程在启动时自动向 machined 注册，
并通过它发现集群内其他进程。

## 验收标准

- [x] machined 进程可启动并监听 TCP 端口 ✅
- [x] 服务器进程启动时自动注册，关闭时自动注销 ✅
- [x] 进程可按类型查询已注册的其他进程地址（含负载信息）✅
- [x] 进程异常退出后 machined 在 ~15 秒内检测到并广播死亡通知（TCP keepalive + 心跳超时）✅
- [x] Birth/Death Listener 机制可工作：进程可订阅其他类型进程的上下线事件 ✅
- [x] 进程唯一性校验：Manager 类型同类只允许一个，普通进程同类型+同名不允许 ✅
- [x] Watcher 查询可通过 machined 转发到目标进程 ✅
- [x] machined 优雅关闭时通知所有已注册进程 ✅
- [x] 单元测试覆盖注册/注销/查询/超时/监听全部场景 ✅
- [x] 集成测试: 单进程内嵌多 EventDispatcher，两个模拟进程通过 machined 互相发现 ✅
- [~] `atlas_tool` CLI 工具可列出进程、查询 Watcher 🚧（`list` 可用，`watch` 仍是占位）

## 验收状态（2026-04-18）

- `machined` 当前实现是 `TCP 注册/查询 + UDP heartbeat` 的混合模型，而不是早期草案中的纯 TCP 或 BigWorld 式广播 UDP。
- 服务端的 Watcher 转发链路已经在 `machined`/`WatcherForwarder`/`MachinedClient` 中落地。
- `atlas_tool list` 已可用，但 `atlas_tool watch` 仍是占位提示，尚未把原始 `WatcherRequest` 能力暴露给 CLI。

---

## 1. BigWorld 架构分析与 Atlas 适配 ✅

已完成架构分析与决策：Atlas 采用 TCP 长连接 + 配置文件地址（替代 BigWorld 的 UDP 广播），死亡检测采用激进 TCP keepalive（~8s）+ 应用层心跳超时（15s）双保险，消息协议复用 Atlas `NetworkMessage` concept。进程启动不实现（由 systemd/K8s/手动编排）。

---

## 2. 消息协议设计 ✅

全部 15 个消息类型已落地于 `src/lib/network/machined_types.h`，含 `static_assert(NetworkMessage<T>)` 验证：
`RegisterMessage` / `RegisterAck` / `DeregisterMessage` / `QueryMessage` / `QueryResponse` /
`HeartbeatMessage` / `HeartbeatAck` / `BirthNotification` / `DeathNotification` /
`ListenerRegister` / `ListenerAck` / `WatcherRequest` / `WatcherResponse` / `WatcherForward` / `WatcherReply`。
协议版本号字段已引入。消息往返测试见 `tests/unit/test_machined_types.cpp`。

---

## 3. 核心模块设计 ✅

### 3.1 ProcessRegistry — 进程注册表 ✅

`src/server/machined/process_registry.{h,cc}` 已实现。支持按 PID / ChannelId / ProcessType 三向索引，负载更新，Manager 类型唯一性 + 普通进程同名拒绝 + 端口冲突 + PID 重复校验齐备。

### 3.2 ListenerManager — 事件监听管理 ✅

`src/server/machined/listener_manager.{h,cc}` 已实现。Birth/Death/Both 订阅、按 target_type 分桶、ChannelId 反向索引清理。

### 3.3 WatcherForwarder — Watcher 查询转发 ✅

`src/server/machined/watcher_forwarder.{h,cc}` 已实现。请求路由、超时清理、找不到目标时返回 not_found。

### 3.4 MachinedApp — machined 进程主类 ✅

`src/server/machined/machined_app.{h,cc}` 已实现，继承 `ManagerApp`。TCP 接入、全部消息 handler、`OnDisconnect` 触发 Death 通知、`OnTickComplete` 中心跳超时与 watcher 超时检查、`RegisterWatchers` 暴露 machined 自身运行指标、SIGTERM/SIGINT 优雅关闭均已落地。当前实现包含 UDP heartbeat 端口以卸载 TCP 上的心跳流量。

---

## 4. MachinedClient — 客户端库 (`src/lib/server/`) ✅

### 4.1 设计 ✅

`src/lib/server/machined_client.{h,cc}` 已实现。`Connect` / `SendRegister` / `SendDeregister` / `SendHeartbeat` / `QuerySync` / `QueryAsync` / `Subscribe`（Birth/Death 回调）、`Tick`（5s 间隔心跳调度）、UDP heartbeat 地址切换、`Register*Handlers` 消息注册均已落地。

### 4.2 ServerApp 集成 ✅

`ServerApp` 在 `init()` 中连接 machined 并注册，`fini()` 中反注册。machined 连接失败采用降级策略（非致命，允许单机开发模式）。

### 4.3 断线重连 🚧

基础 `Connect` 路径已可用；当前 `MachinedClient` 尚未内置自动重连定时器与重注册流程，仍依赖调用方处理。TODO：补充 `OnDisconnect` 下的 reconnect backoff + 监听恢复。

---

## 5. 进程发现与 Birth/Death 典型流程 ✅

BaseApp 启动注册、异常退出触发 Death、Manager 查询带重试的时序图对应实现均已在 `machined_app` / `machined_client` / 相关集成测试中验证。

---

## 6. Watcher 查询转发流程 ✅

`WatcherRequest` → `WatcherForward` → 目标进程 → `WatcherReply` → `WatcherResponse` 链路已打通，由 `WatcherForwarder` 维护 pending 表并做超时清理。`MachinedClient::OnWatcherResponse` 可接收回执（但 `atlas_tool watch` 子命令尚未接入，见 Step 6.9）。

---

## 7. 实现步骤

- **Step 6.1 消息定义扩展** ✅ — `machined_types.h` 全部消息 + `test_machined_types.cpp` 已提交。
- **Step 6.2 ProcessRegistry** ✅ — `process_registry.{h,cc}` + `test_process_registry.cpp`。
- **Step 6.3 ListenerManager** ✅ — `listener_manager.{h,cc}` + `test_listener_manager.cpp`。
- **Step 6.4 WatcherForwarder** ✅ — `watcher_forwarder.{h,cc}` + `test_watcher_forwarder.cpp`。
- **Step 6.5 MachinedApp — machined 进程** ✅ — `machined_app.{h,cc}` + `main.cc` + `CMakeLists.txt`；自身 Watcher 指标已注册。
- **Step 6.6 MachinedClient — 客户端库** ✅（重连除外）— `machined_client.{h,cc}`；断线重连仍为 TODO。
- **Step 6.7 ServerApp 集成** ✅ — `server_app.{h,cc}` 已接入 `MachinedClient`，失败降级。
- **Step 6.8 集成测试** ✅ — `tests/integration/test_machined_registration.cpp`（340 行，覆盖注册/查询/Birth-Death/心跳超时端到端场景）。
- **Step 6.9 atlas_tool CLI 运维工具** 🚧 — `src/tools/atlas_tool/main.cc` 已实现 `list [type]`；`watch <target> <path>` 仍是占位提示，需要在 `MachinedClient` 暴露 raw `WatcherRequest` 并接入；`shutdown` 子命令未实现。

---

## 8. 文件清单汇总

```
src/lib/network/
└── machined_types.h                   ✅

src/lib/server/
├── machined_client.{h,cc}             ✅ (reconnect TODO)
└── server_app.{h,cc}                  ✅ (MachinedClient 集成)

src/server/machined/
├── CMakeLists.txt                     ✅
├── main.cc                            ✅
├── machined_app.{h,cc}                ✅
├── process_registry.{h,cc}            ✅
├── listener_manager.{h,cc}            ✅
└── watcher_forwarder.{h,cc}           ✅

src/tools/atlas_tool/
└── main.cc                            🚧 (list 完成, watch/shutdown 未完成)

tests/unit/
├── test_machined_types.cpp            ✅
├── test_process_registry.cpp          ✅
├── test_listener_manager.cpp          ✅
└── test_watcher_forwarder.cpp         ✅

tests/integration/
└── test_machined_registration.cpp     ✅
```

---

## 9. 依赖关系与执行顺序 ✅

所有依赖项目已按文档原计划顺序落地，当前仅剩 `atlas_tool` 的 watcher/shutdown 子命令与 `MachinedClient` 断线重连两项收尾工作。

---

## 10. BigWorld 完整对照 ✅

Atlas 的设计差异（TCP vs UDP、心跳+keepalive 双保险、ChannelId vs 裸指针、PID+类型+名称唯一性校验、协议版本号等）均已在实现中贯彻。详见 §1.1–§1.3 的决策背景。

---

## 11. 关键设计决策记录 ✅

已固化的设计决策（实现与之一致）：

1. **TCP vs UDP**：选 TCP，适配容器/云部署，连接状态天然心跳。
2. **machined 连接失败策略**：非致命，降级运行（单机开发模式）。
3. **ProcessType 与 Phase 5 统一**：`machined_types.h` 复用 `server_config.h` 的 `ProcessType` 枚举。
4. **崩溃检测双保险**：TCP keepalive (IDLE=5s, INTVL=1s, CNT=3) + 应用层心跳超时 (5s 发送 / 15s 超时)。
5. **C# 脚本层无关**：machined 纯 C++ 进程（`ManagerApp` 基类）。
6. **多 machined 扩展空间已保留**：`ServerConfig::machined_address` 未来可扩展为列表；当前只实现单 machined。
7. **协议版本号**：`RegisterMessage.protocol_version = 1` 已落地。
8. **ChannelId vs Channel\***：`ProcessRegistry` 使用 Channel 指针 + `OnDisconnect` 明确清理顺序（实现与草案略有简化，功能等价）。
9. **同步查询 API 的安全性**：`QuerySync` 仅在 `init()` 阶段使用，`QueryAsync` 供运行时使用。
10. **进程唯一性校验**：Manager 类型单实例 + 普通进程同类型+同名拒绝，已在 `ProcessRegistry::Register` 中实现。
11. **machined 优雅关闭**：SIGTERM/SIGINT 通知所有已注册进程后延迟关闭。

---

## 收尾 TODO 汇总

- [ ] `MachinedClient` 断线重连 + 自动重注册 + 订阅恢复
- [ ] `atlas_tool watch <target> <path>`：通过 `MachinedClient` 暴露 raw `WatcherRequest`
- [ ] `atlas_tool shutdown <target>`：通过 machined 转发 shutdown 请求
