# Phase 9: LoginApp + BaseAppMgr — 登录流程

**Status:** ✅ 功能主线完成 / ✅ 端到端延迟观测就位。
**前置依赖:** Phase 8 (BaseApp)、Phase 7 (DBApp)、Phase 6 (machined)
**BigWorld 参考:** `server/loginapp/`, `server/baseappmgr/`,
`lib/connection/login_interface.hpp`

多 `LoginApp` / `LoginAppMgr` 扩展设计见
[login_multi_loginapp_design.md](login_multi_loginapp_design.md)。

## 目标

单集群登录主线：`LoginApp` 接受客户端 → `DBApp` 认证 → `BaseAppMgr` 分配
`BaseApp` → `BaseApp` 完成 `PrepareLogin` 与 `Authenticate`。

## 模块职责

| 模块 | 职责 |
|---|---|
| `LoginApp` | 接受客户端 RUDP；速率限制（per-IP + global + trusted CIDR bypass）；用户名去重；驱动主链路；超时与 abandoned login 清理 |
| `BaseAppMgr` | BaseApp 注册；`app_id` / `EntityID` 区间分配；扩展负载跟踪；带 affinity 的分配；过载保护；Global Bases |
| `BaseApp` | `PrepareLogin` 接收；checkout 冲突收敛；detached proxy grace；prepared / pending / deferred login 管理；`Authenticate(session_key)` 认领 |
| `DBApp` | `AuthLogin` 认证；`auto_create_accounts`；checkout / checkin / abort 协调 |

## 协议与字段

- 消息 ID `5000–5008`：`LoginRequest / LoginResult / AuthLogin /
  AuthLoginResult / AllocateBaseApp / AllocateBaseAppResult /
  PrepareLogin / PrepareLoginResult / CancelPrepareLogin`
- `LoginStatus` 状态码：`Success / InvalidCredentials / AlreadyLoggedIn /
  ServerFull / RateLimited / ServerNotReady / InternalError /
  LoginInProgress / ServerBusy`
- `PrepareLogin` 字段：`request_id / type_id / dbid / session_key /
  client_addr / blob_prefetched / entity_blob`
- `BaseAppMgr::InformLoad` 扩展字段：`load / entity_count / proxy_count /
  pending_prepare_count / pending_force_logoff_count / detached_proxy_count
  / logoff_in_flight_count / deferred_login_count`
- `BaseAppMgr` 分配策略：综合 `measured_load / effective_load / queue
  pressure / dbid affinity / overload gate`

## 登录侧关键复杂度

`SessionKey`（32 字节随机值，客户端凭此认领 BaseApp）；checkout 冲突处理；
detached proxy grace；本地重登快路径；远端 `ForceLogoff` + `ForceLogoffAck`
重试；deferred checkout / retry；`CancelPrepareLogin` 回滚；本地 + 跨 BaseApp
重复登录处理。

## 延迟观测

各进程通过 `WatcherRegistry` 暴露登录链路各腿的延迟直方图（µs，
log-linear，~12.5% 相对分辨率），watcher 路径形如
`{prefix}/{count,p50_us,p95_us,p99_us,max_us}`。长尾告警阈值由外部
Prometheus / Grafana 消费，不在 server 进程内做。压测方法见
[../stress_test/LOGIN_STRESS_TESTING.md](../stress_test/LOGIN_STRESS_TESTING.md)。

| watcher 前缀 | 测量起止 |
|---|---|
| `loginapp/login_latency` | `LoginRequest` 接受 → `LoginResult` 发出（成功路径） |
| `baseapp/prepare_login_latency` | `PrepareLogin` 进入 → `PrepareLoginResult(success)` 发出 |
| `baseapp/authenticate_latency` | `Authenticate` 进入 → `AuthenticateResult(success)` 发出 |
| `baseapp/force_logoff_latency` | 远端 `ForceLogoff` 发出 → `ForceLogoffAck(success)` 收到 |
| `dbapp/checkout_reply_latency` | `CheckoutEntity` 入队 → `CheckoutEntityAck` 发出（含冲突回复） |
| `dbapp/write_reply_latency` | `WriteEntity` 入队 → `WriteEntityAck` 发出（覆盖 logoff/writeback 两路） |

## 边界

**本文档不再负责**（统一见
[login_multi_loginapp_design.md](login_multi_loginapp_design.md)）：

- 多 `LoginApp` 入口与 `LoginAppMgr` 外部暴露方案
- `route_token`
- `LoginAppMgr` ingress / coordinator 双模式切换
- 全局排队系统
