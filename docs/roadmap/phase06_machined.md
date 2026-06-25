# Phase 6: machined — 进程管理与服务发现

**Status:** ✅ 完成。基础注册、查询、Watcher 转发和 Shutdown 控制已稳定；
当前部署模型已由 Phase 13 扩展为 per-host machined mesh。
**前置依赖:** Phase 5 (服务器框架基类)
**BigWorld 参考:** `server/tools/bwmachined/`, `lib/network/machine_guard.hpp`,
`lib/network/machined_utils.hpp`

## 目标

集群基石服务 `machined` + 配套客户端库 `MachinedClient`。每台机器跑一个
machined 守护进程，提供进程注册、服务发现、存活检测、事件通知。所有
服务器进程启动时自动向 machined 注册，通过它发现集群内其他进程。

## 设计

### 传输模型

本机进程与本机 `machined` 使用 **TCP 长连接** 承载注册、查询、订阅、Watcher
和 shutdown 控制；`RegisterAck` 会下发 UDP heartbeat 端口，客户端优先用 UDP
发送心跳，未收到端口时回退 TCP 心跳。

跨机器发现使用 **per-host UDP mesh**。每台机器运行一个本地 `machined`，mesh
默认开启，开发 / CI 默认使用 loopback-contained 广播；多机部署需要设置
`--mesh-advertise-ip` 与合适的 `--mesh-broadcast-ip`。mesh 通过 HELLO、
registry gossip 和 process-death datagram 合并远端注册表。`Query` 返回本地
`ProcessRegistry` 与远端 `MeshRegistry` 的合并结果；Watcher / `set-watch` /
`shutdown` 仍只转发给当前 machined 上有本地 TCP channel 的目标。

### 模块

| 模块 | 文件 | 职责 |
|---|---|---|
| `ProcessRegistry` | `process_registry.{h,cc}` | PID / ChannelId / ProcessType 三向索引；负载更新；Manager 类型唯一性 + 同名拒绝 + 端口冲突 + PID 重复校验 |
| `ListenerManager` | `listener_manager.{h,cc}` | Birth/Death/Both 订阅；按 `target_type` 分桶；ChannelId 反向索引清理 |
| `WatcherForwarder` | `watcher_forwarder.{h,cc}` | `WatcherRequest → WatcherForward → WatcherReply → WatcherResponse` 路由 + get/set watcher 转发 + pending 表超时清理 |
| `MachinedApp` | `machined_app.{h,cc}` | 主进程；TCP 接入、消息分发、`OnDisconnect` 触发 Death、`OnTickComplete` 心跳 / watcher 超时检查、SIGTERM/SIGINT 优雅关闭、`ShutdownTarget` 转发 |
| `MachinedClient` | `src/lib/server/machined_client.{h,cc}` | 客户端库；连接 / 注册 / 心跳 / 查询 / 订阅 / `QueryWatcher` / `SetWatcher` / `RequestShutdownTarget`；TCP 断线后指数退避重连 (1→30s) + 自动重注册（订阅在 `RegisterAck` 后回放）；UDP heartbeat 地址切换 |
| `MachinedMeshNode` | `src/lib/server/machined_mesh_node.h` | UDP mesh runtime；周期 HELLO、ring / buddy 失效扫描、registry gossip、process-death 广播 |
| `MeshRegistry` | `src/lib/server/mesh_registry.h` | 按远端 machined owner 缓存远端进程表，供 `Query` 与 birth/death 通知合并 |
| `MeshTransport` | `src/lib/server/mesh_transport.{h,cc}` | 自持 UDP broadcast socket；按 mesh datagram type 派发 |

`ServerApp` 在 `Init()` 中连接 machined 并注册，`Fini()` 中反注册。
machined 连接失败采用降级策略（非致命，允许单机开发模式）。

## 协议

TCP channel 消息定义于 `src/lib/network/machined_types.h`：

`RegisterMessage / RegisterAck / DeregisterMessage / QueryMessage /
QueryResponse / HeartbeatMessage / HeartbeatAck / BirthNotification /
DeathNotification / ListenerRegister / ListenerAck / WatcherRequest /
WatcherResponse / WatcherForward / WatcherReply / ShutdownTarget`

`ShutdownTarget` 由 `atlas_tool` 发起，machined 收到后向匹配的目标进程
转发 `msg::ShutdownRequest`；`ServerApp::Init` 已注册该 handler 调用
`Shutdown()`。

协议版本号字段固定在 `RegisterMessage.protocol_version = 1`。

mesh datagram 定义于 `src/lib/network/mesh_gossip.h`：

`MeshHello / MeshRegistryMsg / MeshProcessDeath`

## 关键设计决策

1. **本机 TCP 控制 + UDP heartbeat + UDP mesh** — TCP channel 仍是本机控制面；
   UDP 用于卸载心跳和跨机器注册表传播。
2. **per-host machined** — 每台机器一个本地 machined，跨机通过 mesh gossip
   合并视图，不再依赖中心 TCP 单例。
3. **连接失败策略** — 非致命，降级运行（单机开发模式）。
4. **`ProcessType` 复用** — `machined_types.h` 直接复用
   `foundation/process_type.h` 的 `ProcessType` 枚举。
5. **崩溃检测双保险** — TCP disconnect / keepalive + 应用层 heartbeat timeout；
   mesh peer 失效由 ring / buddy 扫描广播。
6. **进程唯一性** — Manager 类型单实例；普通进程同类型 + 同名拒绝。
7. **`QuerySync` vs `QueryAsync`** — `QuerySync` 仅在 `Init()` 阶段使用，
   `QueryAsync` 供运行时使用。
8. **远端控制边界** — 查询合并 mesh 远端表；watch / set-watch / shutdown
   只转发到当前 machined 上有本地 channel 的进程。
9. **C# 脚本无关** — machined 是纯 C++ 进程（`ManagerApp` 基类）。
10. **优雅关闭** — SIGTERM/SIGINT 通知所有本地已注册进程后延迟关闭。

## atlas_tool 命令

```
atlas_tool list [type]                  # 列出已注册进程
atlas_tool watch <type[:name]> <path>   # 通过 machined 转发 WatcherRequest
atlas_tool set-watch <type[:name]> <path> <value>
                                        # 写入 read-write watcher，成功后返回当前值
atlas_tool shutdown <type[:name]> [reason]
                                        # 通过 machined 转发 ShutdownRequest；
                                        # 省略 :name 则向 type 全部实例广播
```

`list` / 查询结果合并本地与 mesh 远端进程表。`watch` / `set-watch` 走
`WatcherForwarder`：machined 把请求转发给本地已连接目标进程的 watcher 注册表，
应答经同一通道返回。`set-watch` 只能写 read-write watcher，失败时返回非零退出码。
`shutdown` 在 machined 内查本地表后直接发 `msg::ShutdownRequest`（reason 取自
CLI），目标进程的 `ServerApp` handler 触发 `Shutdown()`。machined 会记住已成功
转发的 reason；目标随后 deregister、disconnect 或 heartbeat timeout 时，对外
`DeathNotification.reason` 保持同一值。
