# Phase 6: machined — 进程管理与服务发现

**Status:** ✅ 完成。
**前置依赖:** Phase 5 (服务器框架基类)
**BigWorld 参考:** `server/tools/bwmachined/`, `lib/network/machine_guard.hpp`,
`lib/network/machined_utils.hpp`

## 目标

集群基石服务 `machined` + 配套客户端库 `MachinedClient`。每台机器跑一个
machined 守护进程，提供进程注册、服务发现、存活检测、事件通知。所有
服务器进程启动时自动向 machined 注册，通过它发现集群内其他进程。

## 设计

### 传输模型

`machined` 采用 **TCP 长连接（注册/查询/控制）+ UDP heartbeat（卸载心跳
流量）** 的混合模型，而非 BigWorld 的纯 UDP 广播。原因：

- 适配容器 / 云部署，TCP 连接状态天然兼任心跳
- 死亡检测双保险：TCP keepalive (IDLE=5s, INTVL=1s, CNT=3) +
  应用层心跳超时（5s 发送 / 15s 超时）
- 进程地址通过配置文件分发（`ServerConfig::machined_address`），未来可
  扩展为多 machined 列表

### 模块

| 模块 | 文件 | 职责 |
|---|---|---|
| `ProcessRegistry` | `process_registry.{h,cc}` | PID / ChannelId / ProcessType 三向索引；负载更新；Manager 类型唯一性 + 同名拒绝 + 端口冲突 + PID 重复校验 |
| `ListenerManager` | `listener_manager.{h,cc}` | Birth/Death/Both 订阅；按 `target_type` 分桶；ChannelId 反向索引清理 |
| `WatcherForwarder` | `watcher_forwarder.{h,cc}` | `WatcherRequest → WatcherForward → WatcherReply → WatcherResponse` 路由 + pending 表超时清理 |
| `MachinedApp` | `machined_app.{h,cc}` | 主进程；TCP 接入、消息分发、`OnDisconnect` 触发 Death、`OnTickComplete` 心跳 / watcher 超时检查、SIGTERM/SIGINT 优雅关闭、`ShutdownTarget` 转发 |
| `MachinedClient` | `src/lib/server/machined_client.{h,cc}` | 客户端库；连接 / 注册 / 心跳 / 查询 / 订阅 / `QueryWatcher` / `RequestShutdownTarget`；TCP 断线后指数退避重连 (1→30s) + 自动重注册（订阅在 `RegisterAck` 后回放）；UDP heartbeat 地址切换 |

`ServerApp` 在 `Init()` 中连接 machined 并注册，`Fini()` 中反注册。
machined 连接失败采用降级策略（非致命，允许单机开发模式）。

## 协议

16 个消息类型定义于 `src/lib/network/machined_types.h`：

`RegisterMessage / RegisterAck / DeregisterMessage / QueryMessage /
QueryResponse / HeartbeatMessage / HeartbeatAck / BirthNotification /
DeathNotification / ListenerRegister / ListenerAck / WatcherRequest /
WatcherResponse / WatcherForward / WatcherReply / ShutdownTarget`

`ShutdownTarget` 由 `atlas_tool` 发起，machined 收到后向匹配的目标进程
转发 `msg::ShutdownRequest`；`ServerApp::Init` 已注册该 handler 调用
`Shutdown()`。

协议版本号字段固定在 `RegisterMessage.protocol_version = 1`。

## 关键设计决策

1. **TCP vs UDP** — 选 TCP。容器 / 云部署友好，连接状态即心跳。
2. **连接失败策略** — 非致命，降级运行（单机开发模式）。
3. **`ProcessType` 复用** — `machined_types.h` 直接复用 `server_config.h` 的
   `ProcessType` 枚举。
4. **崩溃检测双保险** — TCP keepalive + 应用层心跳超时。
5. **进程唯一性** — Manager 类型单实例；普通进程同类型 + 同名拒绝。
6. **`QuerySync` vs `QueryAsync`** — `QuerySync` 仅在 `Init()` 阶段使用，
   `QueryAsync` 供运行时使用。
7. **C# 脚本无关** — machined 是纯 C++ 进程（`ManagerApp` 基类）。
8. **优雅关闭** — SIGTERM/SIGINT 通知所有已注册进程后延迟关闭。

## atlas_tool 命令

```
atlas_tool list [type]                  # 列出已注册进程
atlas_tool watch <type[:name]> <path>   # 通过 machined 转发 WatcherRequest
atlas_tool shutdown <type[:name]> [reason]
                                        # 通过 machined 转发 ShutdownRequest；
                                        # 省略 :name 则向 type 全部实例广播
```

`watch` 走 `WatcherForwarder`：machined 把请求转发给目标进程的 watcher
注册表，应答经同一通道返回。`shutdown` 在 machined 内查表后直接发
`msg::ShutdownRequest`（reason 取自 CLI），目标进程的 `ServerApp` handler
触发 `Shutdown()`。
