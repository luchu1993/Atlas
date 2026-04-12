# Multi-LoginApp + LoginAppMgr 设计方案

> 日期: 2026-04-12
> 状态: 修订草案
> 适用范围: Atlas 多 LoginApp 扩展
> 说明: 本文档是 `LoginAppMgr` 详细设计的 source of truth。若与 `docs/roadmap/phase09_login_flow.md`
> 的早期占位描述冲突，以本文为准。

## 1. 设计结论

本次修订后的核心结论如下:

- `LoginAppMgr` 是**内部控制面**，不直接对客户端开放。
- 客户端入口保持为 `LoginApp` 的外部监听地址，由 **L4 LB / DNS RR / K8s Service** 做入口分发。
- `LoginAppMgr` 的首要职责是:
  - 管理 `LoginApp` 实例注册与健康状态
  - 提供**全局用户名 claim**，保证同一用户名在全集群只有一个活跃登录流程
  - 聚合 `LoginApp` 负载与可观测性，为后续 admission / queue 能力预留扩展点
- 生产环境默认采用 **fail-closed** 策略:
  - `LoginAppMgr` 不可用时，新的登录请求返回 `ServerNotReady`
  - 不允许自动回退到“多实例下仅本地去重”的模式
- 客户端可见的**全局排队系统**不纳入本期 MVP。该能力只有在具备 ticket/resume 语义、
  幂等入队协议和高可用控制面之后才进入下一阶段。

这份设计的目标不是“最少改动”，而是以工业级约束为前提，给出一个可以稳定上线、可演进、
可运维的实现边界。

## 2. 背景与约束

当前 Atlas 已具备:

- `LoginApp` 外部 RUDP 监听
- `LoginApp -> DBApp -> BaseAppMgr -> BaseApp` 登录编排链路
- `BaseAppMgr` 风格的 manager 进程模式
- `ChannelId` 优先、避免跨异步边界存储 `Channel*` 裸指针

当前单实例 `LoginApp` 的问题主要有:

- 所有登录请求集中在一个进程上
- 用户名去重只在本地实例内有效
- 登录 admission 只能做局部判断

但工业级实现还必须满足以下非功能约束:

- **公网入口和内部控制面隔离**
- **故障时语义明确**
- **协议幂等、可校验所有权**
- **文档与现有代码模式对齐**

## 3. 总体架构

### 3.1 拓扑

```text
                      Public / Internet
                              |
                    +-------------------+
                    |  L4 LB / DNS RR   |
                    +---------+---------+
                              |
          +-------------------+-------------------+
          |                   |                   |
   +------v------+     +------v------+     +------v------+
   | LoginApp-1  |     | LoginApp-2  | ... | LoginApp-N  |
   | ext + int   |     | ext + int   |     | ext + int   |
   +------+------+     +------+------+     +------+------+
          |                   |                   |
          +-------------------+-------------------+
                              |
                    +---------v---------+
                    |   LoginAppMgr     |
                    | internal only     |
                    | claim + registry  |
                    +---------+---------+
                              |
               +--------------+--------------+
               |                             |
        +------v------+               +------v------+
        |   DBApp     |               | BaseAppMgr  |
        +-------------+               +-------------+
```

### 3.2 边界定义

- 客户端**只**连接 `LoginApp` 的外部地址。
- 客户端**不会**连接 `machined`。
- 客户端**不会**连接 `LoginAppMgr`。
- `LoginAppMgr` 只通过集群内部 `RUDP` 与各 `LoginApp` 通信。
- `machined` 只负责服务发现和 Birth/Death 通知，不承担客户端服务发现职责。

### 3.3 为什么这样设计

这套拓扑有几个直接收益:

- 避免把 manager 进程变成公网入口和新的容量瓶颈
- 复用现有 `LoginApp` 的外部 RUDP 监听、断开回调、超时和消息处理模型
- 让 `LoginAppMgr` 保持和 `BaseAppMgr` 相同的“控制面”定位
- 方便后续接入标准基础设施: Nginx stream、Envoy、K8s Service、SLB、Anycast 等

## 4. 非目标

本期不做以下事情:

- 不做 `Client -> LoginAppMgr -> LoginApp` 的两段式跳转
- 不做 `LoginAppMgr` 透明代理客户端流量
- 不做无状态、无 token 的客户端排队
- 不在生产模式下支持 `LoginAppMgr` 故障时自动退化到多实例本地去重

这些方案要么与现有 Atlas 实现风格冲突，要么在一致性与运维上不达工业级标准。

## 5. 生产模式与降级策略

### 5.1 模式定义

引入两种明确模式:

- `coordinator_required = true`:
  - 生产默认
  - 多 `LoginApp` 部署必须启用
  - `LoginAppMgr` 不健康时拒绝新登录
- `coordinator_required = false`:
  - 仅限开发 / 单机 / 集成测试
  - 仅允许**单 LoginApp 实例**部署
  - 可回退到本地用户名去重

文档明确禁止在“多 `LoginApp` + `coordinator_required = false`”组合下上线生产。

### 5.2 故障语义

| 场景 | 生产策略 |
|------|---------|
| `LoginApp` 崩溃 | 由 LB 摘除，客户端重试到其他实例 |
| `LoginAppMgr` 崩溃/网络分区 | 所有 `LoginApp` 停止接收新登录，返回 `ServerNotReady` |
| `LoginAppMgr` 重启恢复 | `LoginApp` 重新注册，之后恢复接单；**不 replay 旧 claim** |
| `LoginApp` 与 `LoginAppMgr` 间 channel 断开 | 当前进行中的 pending login 立即失败并清理 |

关键原则:

- 生产模式下，宁可短时间拒绝新登录，也不能在多实例之间放弃全局 claim 语义。
- `LoginAppMgr` 重启后不尝试“盲目重放”旧 pending 状态，因为这会引入 app_id /
  registration epoch / lease 竞态。

## 6. LoginAppMgr 职责

`LoginAppMgr` 在 MVP 中只承担三类职责:

1. `LoginApp` 注册与健康状态跟踪
2. 全局用户名 claim / release
3. 负载聚合与 watcher 暴露

它**不负责**:

- 给客户端返回目标 `LoginApp` 地址
- 参与认证、数据库查询、BaseApp 分配、PrepareLogin
- 直接维护客户端连接生命周期

## 7. 协议设计

### 7.1 Message ID 范围

在 `src/lib/network/message_ids.hpp` 中新增:

```cpp
// 9000 - 9099  LoginAppMgr (internal control-plane only)
enum class LoginAppMgr : uint16_t
{
    RegisterLoginApp      = 9000,
    RegisterLoginAppAck   = 9001,
    LoginAppReady         = 9002,
    InformLoginLoad       = 9003,
    ClaimUsername         = 9004,
    ClaimUsernameResult   = 9005,
    RenewUsernameClaim    = 9006,
    ReleaseUsername       = 9007,

    // Reserved for future admission / queue / HA sync
    AdmissionQuery        = 9010,
    AdmissionResult       = 9011,
    QueueEnqueue          = 9020,
    QueueEnqueueResult    = 9021,
    QueueCancel           = 9022,
};
```

说明:

- `9010+` 仅预留，不属于当前实现范围。
- 本期移除原先 `ResolveLoginApp` / `ResolveLoginAppResult` 的客户端入口设计。

### 7.2 Register / load 协议

新增文件: `src/server/loginappmgr/loginappmgr_messages.hpp`

| ID | 消息 | 方向 | 字段 |
|----|------|------|------|
| 9000 | `RegisterLoginApp` | LoginApp -> Mgr | `internal_addr`, `external_addr` |
| 9001 | `RegisterLoginAppAck` | Mgr -> LoginApp | `success`, `app_id`, `registration_epoch` |
| 9002 | `LoginAppReady` | LoginApp -> Mgr | `app_id`, `registration_epoch` |
| 9003 | `InformLoginLoad` | LoginApp -> Mgr | `app_id`, `registration_epoch`, `load`, `pending_count`, `rate_limited_count` |

`registration_epoch` 是本次注册会话的逻辑代次，用于防止:

- 旧 channel 上的延迟包污染新状态
- `LoginAppMgr` 重启后错误接受旧实例消息
- `LoginApp` 重连后旧会话消息误删新会话 claim

### 7.3 Claim 协议

#### ClaimUsername

```cpp
struct ClaimUsername
{
    uint32_t app_id{0};
    uint64_t registration_epoch{0};
    uint32_t request_id{0};
    std::string username;
};
```

#### ClaimUsernameResultStatus

```cpp
enum class ClaimUsernameStatus : uint8_t
{
    Granted = 0,
    AlreadyClaimed = 1,
    CoordinatorUnavailable = 2,
    InvalidRegistration = 3,
};
```

#### ClaimUsernameResult

```cpp
struct ClaimUsernameResult
{
    uint32_t request_id{0};
    ClaimUsernameStatus status{ClaimUsernameStatus::CoordinatorUnavailable};
    uint64_t lease_id{0};
    uint32_t lease_ttl_ms{0};
};
```

#### RenewUsernameClaim

```cpp
struct RenewUsernameClaim
{
    uint32_t app_id{0};
    uint64_t registration_epoch{0};
    uint32_t request_id{0};
    std::string username;
    uint64_t lease_id{0};
};
```

#### ReleaseUsername

```cpp
enum class ReleaseReason : uint8_t
{
    Completed = 0,
    Failed = 1,
    ClientDisconnected = 2,
    Timeout = 3,
    CoordinatorLost = 4,
};

struct ReleaseUsername
{
    uint32_t app_id{0};
    uint64_t registration_epoch{0};
    uint32_t request_id{0};
    std::string username;
    uint64_t lease_id{0};
    ReleaseReason reason{ReleaseReason::Failed};
};
```

### 7.4 协议约束

工业级实现必须满足:

- `ReleaseUsername` 只有在 `(app_id, registration_epoch, request_id, username, lease_id)`
  全部匹配当前 owner 时才生效
- `RenewUsernameClaim` 同样需要 owner 全量匹配
- `ClaimUsernameResult` 不能只返回 `bool success`
- 所有消息都必须校验消息来源:
  - `src == registered internal_addr`
  - `channel_id == registered channel_id`
  - `registration_epoch == current session epoch`

这部分必须复用当前 `BaseAppMgr` 的 source validation 思路，而不是只看 `app_id`。

## 8. Manager 状态模型

### 8.1 LoginAppInfo

```cpp
struct LoginAppInfo
{
    Address internal_addr;
    Address external_addr;
    uint32_t app_id{0};
    uint64_t registration_epoch{0};
    ChannelId channel_id{kInvalidChannelId};

    float measured_load{0.0f};
    uint32_t pending_count{0};
    uint32_t rate_limited_count{0};
    bool is_ready{false};
    TimePoint last_load_report_at{};
};
```

### 8.2 ClaimEntry

```cpp
struct ClaimEntry
{
    uint32_t app_id{0};
    uint64_t registration_epoch{0};
    uint32_t request_id{0};
    uint64_t lease_id{0};
    TimePoint claimed_at{};
    TimePoint expires_at{};
};
```

索引建议:

```cpp
std::unordered_map<std::string, ClaimEntry> claims_by_username_;
std::unordered_map<uint32_t, std::unordered_set<std::string>> usernames_by_app_;
```

设计要求:

- `lease_id` 使用不可预测值
- TTL 必须大于 `LoginApp` pending timeout，并预留网络抖动余量
- 若 pending login 可能超过 TTL，`LoginApp` 需周期性发送 `RenewUsernameClaim`

推荐初始值:

- `LoginApp` pending timeout: 10s
- claim lease TTL: 30s
- renew 周期: 10s

## 9. 登录时序

### 9.1 正常路径

```text
Client         LB          LoginApp-X       LoginAppMgr      DBApp     BaseAppMgr   BaseApp
  |             |               |                |             |           |           |
  |-- connect ->|-------------->|                |             |           |           |
  | LoginRequest|               |                |             |           |           |
  |------------>|               |                |             |           |           |
  |             |               |- ClaimUser --->|             |           |           |
  |             |               |<- Granted -----|             |           |           |
  |             |               |- AuthLogin ----------------->|           |           |
  |             |               |<---------- AuthResult -------|           |           |
  |             |               |- AllocateBase --------------------------->|           |
  |             |               |<---------------- AllocateResult ----------|           |
  |             |               |- PrepareLogin -------------------------------------->|
  |             |               |<--------------------------- PrepareResult -----------|
  |             |               |- ReleaseUser -->|          |           |            |
  |<----------------------------- LoginResult     |          |           |            |
```

### 9.2 LoginApp 处理顺序

`on_login_request()` 的顺序固定为:

1. 本地 rate limit
2. 本地 pending 上限检查
3. 检查 `LoginAppMgr` 是否健康
4. 检查本地 `pending_by_username_`
5. 发送 `ClaimUsername`
6. claim 成功后才进入 `AuthLogin`

这样做有两个目的:

- 降低无效认证请求进入 DBApp
- 保证“登录流程存在”与“用户名全局占用”绑定

### 9.3 失败路径

以下任一路径结束时，`LoginApp` 都必须释放 claim:

- 认证失败
- BaseApp 分配失败
- PrepareLogin 失败
- 客户端断开
- Pending timeout

如果 `LoginAppMgr` channel 在 pending 期间断开:

- 立即 fail 当前 pending login
- 清空本地 pending 记录
- 返回 `ServerNotReady`
- 不继续执行后续 Auth / Allocate / Prepare

## 10. LoginApp 改造要求

### 10.1 新增成员

```cpp
ChannelId loginappmgr_channel_id_{kInvalidChannelId};
uint32_t my_app_id_{0};
uint64_t registration_epoch_{0};
TimePoint last_load_report_{};
TimePoint last_claim_renew_{};
bool coordinator_required_{true};
```

### 10.2 PendingLogin 扩展

```cpp
enum class PendingStage : uint8_t
{
    WaitingClaim = 0,
    WaitingAuth = 1,
    WaitingBaseApp,
    WaitingCheckout,
    WaitingPrepare,
};

struct PendingLogin
{
    uint32_t request_id{0};
    ChannelId client_channel_id{kInvalidChannelId};
    std::string username;
    PendingStage stage{PendingStage::WaitingClaim};
    uint64_t claim_lease_id{0};
    TimePoint claim_expires_at{};
    // existing fields...
};
```

### 10.3 重连策略

`LoginApp` 与 `LoginAppMgr` 重连后:

1. 重新 `RegisterLoginApp`
2. 等待 `RegisterLoginAppAck`
3. 更新 `my_app_id_` 与 `registration_epoch_`
4. 重新发送 `LoginAppReady`
5. 恢复接受新登录

明确禁止:

- 在旧 `app_id` / 旧 `registration_epoch` 下 replay claim
- 对 manager 重启前的 pending login 做“补注册”

工业级实现优先选择**清晰的一致性边界**，而不是复杂且不可靠的状态重放。

## 11. LoginAppMgr 进程实现要求

### 11.1 进程类型

在 `ProcessType` 中新增:

```cpp
LoginAppMgr = 9
```

并补齐:

- `process_type_name()`
- `process_type_from_name()`

### 11.2 基类与网络

`LoginAppMgr` 仍继承 `ManagerApp`，因为它是纯控制面。

但需要明确:

- 只监听 `internal_port`
- 使用 `cluster_rudp_profile()`
- **不新增外部客户端监听口**

### 11.3 Handler 风格

所有 handler 都必须遵循与 `BaseAppMgr` 一致的校验模式:

- 先按 `app_id` 找实例
- 再校验 `src`
- 再校验 `channel_id`
- 再校验 `registration_epoch`

这是一条硬性工程规范，不能像草案那样在 `on_claim_username()` /
`on_release_username()` 中直接信任消息字段。

## 12. 观测与运维

最少需要暴露以下 watcher / metrics:

- `loginappmgr/loginapp_count`
- `loginappmgr/healthy_loginapp_count`
- `loginappmgr/claim_count`
- `loginappmgr/claim_grant_total`
- `loginappmgr/claim_reject_total`
- `loginappmgr/claim_expire_total`
- `loginappmgr/register_total`
- `loginappmgr/source_validation_fail_total`

同时每个 `LoginApp` 至少暴露:

- `loginapp/coordinator_connected`
- `loginapp/coordinator_required`
- `loginapp/pending_claims`
- `loginapp/claim_timeout_total`
- `loginapp/claim_release_total`

LB 健康检查不直接依赖 `LoginAppMgr` 内部状态，而是通过 `LoginApp` 自身健康状态决定是否摘流。
推荐规则:

- `LoginApp` 外部监听正常
- `LoginAppMgr` 已连接
- `DBApp` 已连接
- `BaseAppMgr` 已连接

四者同时满足时才对外宣称 ready。

## 13. 排队系统: 延后到下一阶段

原草案中的全局排队设计存在以下工程问题:

- 客户端排队状态依赖进程内 `ChannelId`
- 无 queue ticket / resume token
- 无幂等入队
- 无断线恢复
- 无 HA 方案

因此本期决策是:

- **P5 不实现客户端可见全局队列**
- 当前过载时继续返回 `ServerBusy` / `ServerNotReady`

未来若要做全局队列，必须先满足以下前置条件:

1. `QueueTicket` 是可恢复、可跨重连复用的显式 token
2. 入队协议幂等
3. 支持取消、超时、重绑定
4. `LoginAppMgr` 至少具备 active-standby 或可恢复快照
5. 客户端队列状态由 `LoginApp` 转发，或引入专门 `LoginGateway`

在这些条件满足前，把“排队”写进 MVP 只会扩大不确定性。

## 14. 实施阶段

按工业级优先级调整为 6 个阶段:

| 阶段 | 内容 | 是否进入当前范围 |
|------|------|------------------|
| P1 | `ProcessType` / message IDs / config 开关 | 是 |
| P2 | `loginappmgr_messages.hpp` + 单元测试 | 是 |
| P3 | `LoginAppMgr` 进程实现: register / validate / claim / renew / release | 是 |
| P4 | `LoginApp` 集成: claim-first 状态机、fail-closed、metrics | 是 |
| P5 | HA 与恢复: active-standby / snapshot / restart procedure | 否，下一阶段 |
| P6 | 全局排队: ticket-based queue | 否，下一阶段 |

### 当前交付标准

本期只有满足以下条件才算完成:

- 多 `LoginApp` 部署下，用户名全局 claim 正确
- `LoginAppMgr` 不可用时，新登录 fail-closed
- 无旧 lease 误删新 claim 的协议漏洞
- 单元测试覆盖 claim ownership / epoch 校验 / TTL 过期
- 集成测试覆盖多实例重复登录竞争

## 15. 变更文件清单

| 操作 | 文件 | 阶段 |
|------|------|------|
| 修改 | `src/lib/server/server_config.hpp` | P1 |
| 修改 | `src/lib/server/server_config.cpp` | P1 |
| 修改 | `src/lib/network/message_ids.hpp` | P1 |
| 修改 | `src/server/CMakeLists.txt` | P3 |
| 新增 | `src/server/loginappmgr/CMakeLists.txt` | P3 |
| 新增 | `src/server/loginappmgr/loginappmgr_messages.hpp` | P2 |
| 新增 | `src/server/loginappmgr/loginappmgr.hpp` | P3 |
| 新增 | `src/server/loginappmgr/loginappmgr.cpp` | P3 |
| 新增 | `src/server/loginappmgr/main.cpp` | P3 |
| 修改 | `src/server/loginapp/loginapp.hpp` | P4 |
| 修改 | `src/server/loginapp/loginapp.cpp` | P4 |
| 修改 | `tests/unit/` 下 LoginAppMgr 消息与 claim 测试 | P2/P3 |
| 修改 | `tests/integration/` 下多 LoginApp 竞争登录测试 | P4 |

## 16. 部署拓扑示例

```text
机器 A: machined + LoginAppMgr + BaseAppMgr + CellAppMgr + DBApp
机器 B: machined + LoginApp-1 + BaseApp-1 + CellApp-1
机器 C: machined + LoginApp-2 + BaseApp-2 + CellApp-2
机器 D: machined + LoginApp-3 + BaseApp-3 + CellApp-3

公网入口:
  SLB / Nginx stream / Envoy / K8s Service
      -> LoginApp-{1..N}.external_port
```

客户端配置只需要知道**统一的登录入口地址**，而不是 `LoginAppMgr` 地址。

## 17. 与旧草案的关键差异

本次修订明确废弃以下旧设计:

- `Client -> LoginAppMgr -> LoginApp` 两段跳转
- `ResolveLoginApp` / `ResolveLoginAppResult`
- `LoginAppMgr` 成为客户端 RUDP 排队入口
- 多实例下 coordinator 故障时自动回退本地去重
- 无 lease / 无 epoch / 无 owner 校验的 claim/release 协议

保留并强化的部分:

- `LoginAppMgr` 作为纯 manager / control-plane 进程
- `ChannelId` 而非 `Channel*`
- 复用 `BaseAppMgr` 式的注册、source validation、watcher 结构
- 分阶段交付，先做正确性，再做 HA 和 queue
