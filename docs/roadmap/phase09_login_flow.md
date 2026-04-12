# Phase 9: LoginApp + BaseAppMgr — 登录流程

> 前置依赖: Phase 8 (BaseApp), Phase 7 (DBApp), Phase 6 (machined)
> BigWorld 参考: `server/loginapp/`, `server/baseappmgr/`, `lib/connection/login_interface.hpp`
> 文档状态: 2026-04-13 当前实现快照

---

## 目标

Phase 9 的目标是完成单集群登录主线:

- `LoginApp` 接受客户端登录请求
- `DBApp` 校验账号并返回 `dbid/type_id`
- `BaseAppMgr` 选择可承载的新 `BaseApp`
- `BaseApp` 完成 `PrepareLogin`、重复登录踢下线与最终 `Authenticate`
- 客户端最终进入游戏世界

本阶段文档现在以**当前代码现状**为准，不再保留大段“尚未落地”的伪代码。

多 `LoginApp` 与 `LoginAppMgr` 的扩展设计已拆到独立文档:

- [../MULTI_LOGINAPP_DESIGN.md](../MULTI_LOGINAPP_DESIGN.md)

---

## 验收状态（2026-04-13）

- [x] `LoginApp` 可接受客户端 `RUDP` 连接并处理 `LoginRequest`
- [x] 登录主链路已实现: `AuthLogin -> AllocateBaseApp -> PrepareLogin -> Authenticate`
- [x] `BaseAppMgr` 已实现 BaseApp 注册、负载上报、Entity ID 区间分配
- [x] `BaseAppMgr` 已实现基于实时负载和 `dbid affinity` 的 BaseApp 选择
- [x] `SessionKey` 机制已实现，客户端需凭 `SessionKey` 认领 `BaseApp`
- [x] 登录速率限制已实现（per-IP + global + trusted CIDR bypass）
- [x] 重复登录处理已实现（本地快路径 + 跨 BaseApp `ForceLogoff`）
- [x] `BaseApp` 过载保护已实现，并反馈到 `BaseAppMgr` 分配决策
- [x] `Global Bases` 注册 / 注销 / 广播已实现
- [~] 定向测试和常规链路已具备继续推进条件，但极端短线重登压测仍未达到目标
- [~] 已补充 `test_login_flow.cpp` 与 DBApp 相关集成测试，但极端 churn / 长稳覆盖仍需继续扩展

说明:

- 当前阶段“功能已落地”不等于“极端 churn 压测完全收敛”
- 登录压力下的残余问题见:
  - [../LOGIN_STRESS_REMAINING_ISSUES_20260412.md](../LOGIN_STRESS_REMAINING_ISSUES_20260412.md)
- 客户端登录中途断开后的回滚协议见:
  - [../LOGIN_ROLLBACK_PROTOCOL_20260412.md](../LOGIN_ROLLBACK_PROTOCOL_20260412.md)

---

## 当前实现状态

### 1. 入口与传输层

当前代码中的客户端登录入口已是**外部 RUDP**，不是早期草案中的 TCP。

- `LoginApp` 使用双 `NetworkInterface`
- 内部服务间通信使用 `network()`
- 客户端连接使用 `external_network_`
- `LoginApp` 对客户端 `RUDP` channel 设置了短 inactivity timeout

这一点与早期 Phase 9 草案相比，已经发生了实质更新。

### 2. 登录主链路

当前实际主链路是:

1. Client -> `LoginApp`: `LoginRequest`
2. `LoginApp` -> `DBApp`: `AuthLogin`
3. `DBApp` -> `LoginApp`: `AuthLoginResult`
4. `LoginApp` -> `BaseAppMgr`: `AllocateBaseApp`
5. `BaseAppMgr` -> `LoginApp`: `AllocateBaseAppResult`
6. `LoginApp` -> `BaseApp`: `PrepareLogin`
7. `BaseApp` -> `LoginApp`: `PrepareLoginResult`
8. `LoginApp` -> Client: `LoginResult`
9. Client -> `BaseApp`: `Authenticate(session_key)`

### 3. 当前重复登录路径

当前实现已经不是“简单重新 checkout 一次”。

`BaseApp` 侧已经引入:

- 本地重登快路径
- `detached proxy grace`
- 跨 `BaseApp` 的 `ForceLogoff`
- `ForceLogoffAck` 后重试 checkout
- `PrepareLogin` 取消与回滚

因此本阶段登录链路的关键复杂度，已经转移到**跨进程生命周期收敛**，而不是基础消息收发。

### 4. 当前主要残余问题

截至 2026-04-13，常规功能已经具备，但在**极端短线重登**压测下仍存在残余问题:

- `timeout_fail`
- `invalid_session`
- `no_dbapp`
- checkout 长时间不释放

详细分析见:

- [../LOGIN_STRESS_REMAINING_ISSUES_20260412.md](../LOGIN_STRESS_REMAINING_ISSUES_20260412.md)

---

## BigWorld 对照与 Atlas 当前决策

| 方面 | 当前 Atlas 决策 | 说明 |
|------|------------------|------|
| 登录协议 | `RUDP` 连接 `LoginApp` | 与当前实现一致 |
| 认证路径 | `LoginApp -> DBApp` | DBApp 负责认证 |
| BaseApp 分配 | `LoginApp -> BaseAppMgr` | 无 DBApp 中转 |
| PrepareLogin | `LoginApp -> BaseApp` | BaseApp 负责创建/恢复登录实体 |
| SessionKey | 32 字节随机值 | 已落地 |
| 速率限制 | per-IP + global | 已落地 |
| 重复登录 | 本地快路径 + 跨 BaseApp `ForceLogoff` | 已落地 |
| 多 LoginApp | 单独设计文档处理 | 不再混在本 Phase 9 文档里 |
| 全局排队 | 本阶段不包含 | 后续按独立设计推进 |

---

## 当前端到端时序

```text
Client           LoginApp          DBApp         BaseAppMgr        BaseApp
  │                 │                │                │                │
  │─[1] RUDP连接 ─→│                │                │                │
  │─[2] Login ─────→│                │                │                │
  │                 │─[3] AuthLogin ─→│                │                │
  │                 │←[4] AuthLoginResult             │                │
  │                 │─[5] AllocateBaseApp ───────────→│                │
  │                 │←[6] AllocateBaseAppResult ─────│                │
  │                 │─[7] PrepareLogin ───────────────────────────────→│
  │                 │←[8] PrepareLoginResult ─────────────────────────│
  │←[9] LoginResult│                                                  │
  │                                                                    │
  │─[10] RUDP连接 ────────────────────────────────────────────────────→│
  │─[11] Authenticate(session_key) ──────────────────────────────────→│
  │←[12] AuthenticateResult ─────────────────────────────────────────│
```

补充说明:

- 当前 `PrepareLogin` 允许携带可选 `entity_blob`
- `LoginApp` 已具备 `CancelPrepareLogin` 回滚路径
- `BaseApp` 内部的登录过程可能触发:
  - checkout 冲突
  - 本地重登快路径
  - 远端 `ForceLogoff`
  - deferred checkout / retry

因此“PrepareLogin”在当前实现里已经不是一个单纯的同步创建动作。

---

## 当前协议快照

### 1. `login::LoginStatus`

当前代码中的状态码为:

```cpp
enum class LoginStatus : uint8_t
{
    Success = 0,
    InvalidCredentials = 1,
    AlreadyLoggedIn = 2,
    ServerFull = 3,
    RateLimited = 4,
    ServerNotReady = 5,
    InternalError = 6,
    LoginInProgress = 7,
    ServerBusy = 8,
};
```

相比早期文档，当前实现已增加:

- `LoginInProgress`
- `ServerBusy`

### 2. `LoginApp` 相关消息

| 消息 | ID | 方向 | 当前状态 |
|------|----|------|----------|
| `LoginRequest` | 5000 | Client -> LoginApp | 已实现 |
| `LoginResult` | 5001 | LoginApp -> Client | 已实现 |
| `AuthLogin` | 5002 | LoginApp -> DBApp | 已实现 |
| `AuthLoginResult` | 5003 | DBApp -> LoginApp | 已实现 |
| `AllocateBaseApp` | 5004 | LoginApp -> BaseAppMgr | 已实现 |
| `AllocateBaseAppResult` | 5005 | BaseAppMgr -> LoginApp | 已实现 |
| `PrepareLogin` | 5006 | LoginApp -> BaseApp | 已实现 |
| `PrepareLoginResult` | 5007 | BaseApp -> LoginApp | 已实现 |
| `CancelPrepareLogin` | 5008 | LoginApp -> BaseApp | 已实现 |

### 3. `PrepareLogin` 当前语义

当前实现中，`PrepareLogin` 包含:

- `request_id`
- `type_id`
- `dbid`
- `session_key`
- `client_addr`
- `blob_prefetched`
- `entity_blob`

这意味着:

- `LoginApp` 与 `BaseApp` 之间已经预留了“携带预取实体快照”的路径
- 即使当前主路径仍以 `BaseApp` 内部 checkout/relogin 协调为主，协议层也不再是最初的最小版本

### 4. `BaseAppMgr::InformLoad` 当前字段

当前 `InformLoad` 已经不是早期文档里只有 `load/entity_count/proxy_count` 的简单上报，而是包含:

- `load`
- `entity_count`
- `proxy_count`
- `pending_prepare_count`
- `pending_force_logoff_count`
- `detached_proxy_count`
- `logoff_in_flight_count`
- `deferred_login_count`

这说明 `BaseAppMgr` 的当前负载均衡决策，已经明确面向登录高压场景做了增强。

---

## 当前模块职责

### 1. LoginApp

当前 `LoginApp` 的职责是:

- 接受客户端 `RUDP` 登录请求
- 做速率限制和本地用户名去重
- 跟踪 pending login 状态
- 驱动 `AuthLogin -> AllocateBaseApp -> PrepareLogin`
- 客户端断开时发送 `CancelPrepareLogin`
- 对超时和 abandoned login 做清理

当前实现特征:

- 使用 `ChannelId` 跟踪客户端连接
- 有 `pending_ / pending_by_username_ / canceled_requests_`
- 对 `DBApp` 与 `BaseAppMgr` 的可用性做动态检测

### 2. BaseAppMgr

当前 `BaseAppMgr` 的职责是:

- 注册/注销 `BaseApp`
- 分配 `app_id` 与 `EntityID` 区间
- 跟踪扩展负载指标
- 选择合适的 `BaseApp`
- 做过载保护
- 维护 `dbid affinity`
- 管理 `Global Bases`

当前分配策略不再只是“最小 CPU / 最小实体数”，而是综合:

- `measured_load`
- `effective_load`
- queue pressure
- `dbid affinity`
- overload gate

### 3. BaseApp

当前 `BaseApp` 在登录链路上的职责包括:

- 接收 `PrepareLogin`
- 处理 checkout 冲突
- 本地/远端强制踢旧登录
- 管理 detached proxy grace
- 处理 `CancelPrepareLogin`
- 管理 prepared login / pending login / deferred checkout
- 最终对客户端 `Authenticate(session_key)` 做认领

换句话说，当前登录复杂度已经大量下沉到了 `BaseApp`。

### 4. DBApp

当前 `DBApp` 的登录相关职责是:

- 处理 `AuthLogin`
- 支持 `auto_create_accounts`
- 协调 checkout / checkin / abort checkout
- 在极端重登场景下承受登录关键路径与写回路径的共同压力

---

## 当前测试与产物

### 1. 已存在的关键测试

当前代码库中与 Phase 9 直接相关的测试包括:

- `tests/unit/test_login_messages.cpp`
- `tests/unit/test_login_rollback.cpp`
- `tests/unit/test_baseappmgr_messages.cpp`
- `tests/unit/test_checkout_manager.cpp`
- `tests/unit/test_entity_manager.cpp`
- `tests/unit/test_xml_database.cpp`
- `tests/unit/test_reliable_udp.cpp`
- `tests/integration/test_baseappmgr_registration.cpp`
- `tests/integration/test_login_flow.cpp`
- `tests/integration/test_dbapp_login_flow.cpp`
- `tests/integration/test_dbapp_checkout_cleanup.cpp`

### 2. 当前测试结论

截至 2026-04-13:

- 定向单元测试和模块级集成测试已具备继续推进条件
- `BaseAppMgr` 注册、负载、消息层已有较完整测试
- 已补充端到端 `test_login_flow.cpp` 与 DBApp 集成测试骨架
- 登录主链路的极端压力稳定性仍需继续验证

### 3. 压测参考

压测执行方法、参数和产物结构，见:

- [../LOGIN_STRESS_TESTING.md](../LOGIN_STRESS_TESTING.md)

残余问题分析，见:

- [../LOGIN_STRESS_REMAINING_ISSUES_20260412.md](../LOGIN_STRESS_REMAINING_ISSUES_20260412.md)

---

## 当前已知问题与后续重点

本阶段当前最主要的剩余问题不是“功能没做完”，而是“极端 churn 下状态收敛还不够稳”。

高优先级后续方向:

1. 把 `force-logoff -> relogin -> checkout -> authenticate` 的长链路继续量化
2. 核查 `BaseApp` 本地状态机是否存在未清理或推进顺序竞争
3. 核查 `DBApp` 在多 BaseApp 压力下的排队与回复长尾
4. 补完整端到端行为测试，而不是继续只靠压测日志猜测

这部分的详细问题分解以:

- [../LOGIN_STRESS_REMAINING_ISSUES_20260412.md](../LOGIN_STRESS_REMAINING_ISSUES_20260412.md)

为准。

---

## 与其他文档的边界

### 本文档负责

- 单 `LoginApp` 基线登录链路
- 当前 `BaseAppMgr` / `BaseApp` / `DBApp` / `LoginApp` 的真实实现状态
- 已落地功能、已知问题和剩余验证项

### 本文档不再负责

- 多 `LoginApp` 入口与 `LoginAppMgr` 外部暴露方案
- `route_token`
- `LoginAppMgr` ingress / coordinator 双模式切换
- 全局排队系统

这些内容统一见:

- [../MULTI_LOGINAPP_DESIGN.md](../MULTI_LOGINAPP_DESIGN.md)

---

## 当前结论

截至 2026-04-12:

- Phase 9 的单实例登录主线已经落地
- `BaseAppMgr` 已从“基础分配器”演进到“带扩展负载语义和 affinity 的分配器”
- `BaseApp` 已承载重复登录、回滚和准备阶段的大部分复杂性
- 系统已经可以继续推进后续多实例设计
- 但在极端短线重登压力下，跨进程生命周期收敛仍未完全达到目标

因此，Phase 9 现在应被视为:

- **功能主线已完成**
- **稳定性与极限场景仍在收敛**
