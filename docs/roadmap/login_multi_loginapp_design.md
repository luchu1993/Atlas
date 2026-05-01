# Multi-LoginApp + LoginAppMgr 设计方案

> 日期: 2026-04-12
> 状态: 修订草案
> 适用范围: Atlas 多 LoginApp 扩展
> 说明: 本文档是多 `LoginApp` / `LoginAppMgr` 的**目标设计草案**，不是当前仓库实现快照。
> 当前代码仍以 [phase09_login_flow.md](phase09_login_flow.md) 中描述的
> “单 `LoginApp` 直接对外”链路为准；若与当前实现冲突，以 Phase 9 文档为准。

## 1. 设计结论

本次修订后的核心结论如下:

- 首个多 `LoginApp` 版本建议优先采用 **`Client -> LoginAppMgr -> LoginApp`** 的入口模式。
- `LoginAppMgr` 同时承担两类职责:
  - **外部入口网关**: 接收客户端 `ResolveLoginApp`
  - **内部协调器**: 管理 `LoginApp` 注册、全局用户名 claim/release、负载聚合
- 未来正式线上项目允许切换到 **`Client -> LB -> LoginApp`** 模式。
- 为了支持未来切换，协议和代码必须从第一天起保留**双模式 ingress 扩展点**:
  - 当前模式: `CoordinatorIngress`
  - 未来模式: `LBIngress`
- 无论入口模式如何切换，`LoginAppMgr` 都继续保留，负责内部协调，不会被删除。
- 当前阶段不实现客户端可见全局排队；先把入口选路、route token、全局 claim 和故障语义做正确。

这份设计的目标不是最省代码，而是先把“首个多 LoginApp 版本可上线的入口方案”与“未来可平滑切到 LB 的扩展点”
一起设计好，避免下一轮推翻协议。

## 1.1 当前代码基线

截至 2026-04-12，仓库当前已落地的仍然是:

- 客户端直接连接 `LoginApp`
- `LoginApp` 直接处理 `LoginRequest`
- `LoginAppMgr` 尚未实现
- `ResolveLoginApp` / `route_token` / 全局 username claim 尚未进入当前 wire contract

因此本文后续所有“当前阶段”表述，都应理解为:

- **首个多 LoginApp 实现阶段的目标设计**
- 不是当前 `main` 分支已经存在的行为

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

当前阶段的新增目标是:

- 引入多个 `LoginApp`
- 由 `LoginAppMgr` 统一暴露登录入口
- 在全集群内保证用户名登录流程互斥
- 未来切到 `LB` 时，尽量不改动登录主协议和 `LoginApp` 核心状态机

工业级实现仍需满足这些约束:

- 外部入口协议和内部控制面协议必须隔离
- 语义必须明确到故障场景
- claim/release 必须具备 owner 校验和幂等语义
- 当前入口方案不能把未来 `LB` 迁移路径堵死

## 3. 总体架构

### 3.1 当前阶段拓扑: CoordinatorIngress

```text
                       Public / Internet
                               |
                       +-------v--------+
                       |  LoginAppMgr   |
                       | ext + int      |
                       | ingress +      |
                       | coordinator    |
                       +-------+--------+
                               |
             +-----------------+-----------------+
             |                 |                 |
      +------v------+   +------v------+   +------v------+
      | LoginApp-1  |   | LoginApp-2  |   | LoginApp-N  |
      | internal    |   | internal    |   | internal    |
      +------+------+   +------+------+   +------+------+
             |                 |                 |
             +-----------------+-----------------+
                               |
                    +----------+----------+
                    |                     |
             +------v------+       +------v------+
             |   DBApp     |       | BaseAppMgr  |
             +-------------+       +-------------+
```

### 3.2 未来拓扑: LBIngress

```text
                       Public / Internet
                               |
                    +----------v----------+
                    |   L4 UDP LB / VIP   |
                    +----------+----------+
                               |
         +---------------------+---------------------+
         |                     |                     |
  +------v------+       +------v------+       +------v------+
  | LoginApp-1  |       | LoginApp-2  |       | LoginApp-N  |
  | ext + int   |       | ext + int   |       | ext + int   |
  +------+------+       +------+------+       +------+------+
         |                     |                     |
         +---------------------+---------------------+
                               |
                    +----------v----------+
                    |    LoginAppMgr      |
                    |   internal only     |
                    +----------+----------+
                               |
                    +----------+----------+
                    |                     |
             +------v------+       +------v------+
             |   DBApp     |       | BaseAppMgr  |
             +-------------+       +-------------+
```

### 3.3 架构决策

当前阶段选择 `CoordinatorIngress` 的原因:

- 便于在 Atlas 内部自洽地完成多 `LoginApp` 入口调度
- 当前不依赖额外基础设施即可验证多实例方案
- 可以把入口选择和全局 claim 一起收敛在同一个 manager 中

但本文同时要求:

- `LoginAppMgr` 的**入口功能**与**协调功能**在实现上分层
- 将来切到 `LBIngress` 时，只禁用入口层，不重写 claim 协调层

## 4. 模式与边界

### 4.1 Ingress 模式

新增配置:

```text
login_ingress_mode = coordinator | lb
```

语义如下:

- `coordinator`
  - 当前阶段默认
  - 客户端先连 `LoginAppMgr`
  - `LoginAppMgr` 返回目标 `LoginApp` 地址和 `route_token`
- `lb`
  - 未来线上模式
  - 客户端直接连外部 `LB` 或统一入口
  - `LoginAppMgr` 不再承担客户端入口，但继续承担 claim 协调

### 4.2 `route_token` 兼容策略

为了支持未来切换，`LoginRequest` 中新增可选字段 `route_token`。

模式行为:

- `coordinator` 模式:
  - `route_token` 必须存在且合法
- `lb` 模式:
  - `route_token` 可为空
  - 若存在，可做兼容校验；若为空，直接进入 claim

这意味着:

- 当前客户端可以走两段式
- 未来切到 `LB` 后，客户端协议不必整体推翻

### 4.3 外部入口与内部控制面的隔离

即使当前让 `LoginAppMgr` 对外暴露入口，也必须保持两张网络面:

- `internal network`
  - `ManagerApp` 负责
  - 用于 `RegisterLoginApp`、`ClaimUsername` 等控制消息
- `external network`
  - `LoginAppMgr` 自己维护
  - 只处理客户端 `ResolveLoginApp`

明确禁止:

- 客户端直接向 `LoginAppMgr` 发送内部 claim/control-plane 消息
- `LoginApp` 从外部网络接收 coordinator 内部协议

## 5. 生产模式与故障语义

### 5.1 模式定义

新增配置:

```text
coordinator_required = true | false
require_route_token = true | false
```

推荐组合:

- 当前阶段生产验证:
  - `login_ingress_mode = coordinator`
  - `coordinator_required = true`
  - `require_route_token = true`
- 未来 LB 模式:
  - `login_ingress_mode = lb`
  - `coordinator_required = true`
  - `require_route_token = false`

### 5.2 故障语义

| 场景 | 当前阶段策略 |
|------|--------------|
| `LoginApp` 崩溃 | `LoginAppMgr` 不再给它分配新 route；客户端重新 `ResolveLoginApp` |
| `LoginAppMgr` 崩溃/网络分区 | 新登录不可用；客户端无法 resolve；进行中的登录 fail-closed |
| 客户端拿到 `route_token` 后目标 `LoginApp` 下线 | 客户端重新 `ResolveLoginApp` |
| `LoginApp` 与 `LoginAppMgr` 的内部 channel 断开 | 当前 pending login 立即失败并清理 |
| `LoginAppMgr` 重启恢复 | 各 `LoginApp` 重新注册；旧 route token/claim 全部失效 |

关键原则:

- 当前阶段承认 `LoginAppMgr` 是入口单点。
- 生产模式下绝不自动退化到“多实例本地去重”。
- manager 重启后不 replay 旧 claim，避免引入 epoch/lease 竞态。

## 6. LoginAppMgr 职责

### 6.1 当前阶段职责

`LoginAppMgr` 负责五类事情:

1. `LoginApp` 注册与健康状态跟踪
2. 对外处理 `ResolveLoginApp`
3. 管理短期 `route_token`
4. 管理全局用户名 claim/release
5. 聚合 `LoginApp` 负载并暴露 watcher

### 6.2 非职责

`LoginAppMgr` 仍然不负责:

- 登录认证
- 数据库查询
- BaseApp 分配
- PrepareLogin
- 代理客户端完整登录流量

它只负责**选择目标 LoginApp**，然后让客户端与该 `LoginApp` 直连。

## 7. 登录流程设计

### 7.1 正常路径

```text
Client        LoginAppMgr        LoginApp-X        DBApp     BaseAppMgr   BaseApp
  |                |                 |               |           |           |
  |- Resolve ----->|                 |               |           |           |
  |                |- select app --> |               |           |           |
  |<- route_token -|                 |               |           |           |
  |                |                 |               |           |           |
  |--- LoginRequest(route_token) --->|               |           |           |
  |                |                 |- ClaimUser -->|           |           |
  |                |                 |<- Granted ----|           |           |
  |                |                 |- AuthLogin -------------->|           |
  |                |                 |<---- AuthLoginResult -----|           |
  |                |                 |- AllocateBase ----------------------->|
  |                |                 |<---------------- AllocateResult ------|
  |                |                 |- PrepareLogin ------------------------------>|
  |                |                 |<-------------------------- PrepareResult ----|
  |                |                 |- ReleaseUser ->|         |                  |
  |<--------------- LoginResult -----|               |         |                  |
```

### 7.2 关键语义

流程被拆成两个阶段:

1. `ResolveLoginApp`
  - 只分配一个**短期路由租约**
  - 不代表该用户名已经获得全局登录权
2. `ClaimUsername`
  - 由目标 `LoginApp` 在收到 `LoginRequest` 后发起
  - 只有 claim 成功，登录流程才正式开始

这样拆分的意义:

- 客户端只拿地址但不继续登录时，不会直接污染全局 claim
- 目标 `LoginApp` 仍然是登录状态机的 owner
- 未来切到 `LB` 时，可以删掉 `Resolve`，保留 claim

## 8. 协议设计

### 8.1 Message ID 范围

在 `src/lib/network/message_ids.hpp` 中新增:

```cpp
// 9000 - 9099  LoginAppMgr
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

    ResolveLoginApp       = 9010,
    ResolveLoginAppResult = 9011,
};
```

说明:

- `9010+` 当前属于外部入口协议
- 将来切到 `LB` 模式后，这部分协议可以保留兼容，但不再是主路径

### 8.2 Register / load 协议

新增文件: `src/server/loginappmgr/loginappmgr_messages.hpp`

| ID | 消息 | 方向 | 字段 |
|----|------|------|------|
| 9000 | `RegisterLoginApp` | LoginApp -> Mgr | `internal_addr`, `external_addr` |
| 9001 | `RegisterLoginAppAck` | Mgr -> LoginApp | `success`, `app_id`, `registration_epoch` |
| 9002 | `LoginAppReady` | LoginApp -> Mgr | `app_id`, `registration_epoch` |
| 9003 | `InformLoginLoad` | LoginApp -> Mgr | `app_id`, `registration_epoch`, `load`, `pending_count`, `rate_limited_count` |

`registration_epoch` 是本次注册会话代次，用于防止:

- 旧 channel 上的延迟包污染新状态
- manager 重启后错误接受旧实例消息
- 旧会话的 release/renew 误删新会话 claim

### 8.3 外部入口协议

#### ResolveLoginApp

```cpp
struct ResolveLoginApp
{
    uint32_t protocol_version{0};
    std::string client_version;
};
```

#### ResolveStatus

```cpp
enum class ResolveStatus : uint8_t
{
    Success = 0,
    NoLoginApp = 1,
    ServerBusy = 2,
    ServerNotReady = 3,
};
```

#### ResolveLoginAppResult

```cpp
struct ResolveLoginAppResult
{
    ResolveStatus status{ResolveStatus::ServerNotReady};
    Address loginapp_addr;
    uint64_t route_token{0};
    uint32_t route_ttl_ms{0};
    uint32_t retry_after_ms{0};
};
```

### 8.4 Claim 协议

#### ClaimUsername

```cpp
struct ClaimUsername
{
    uint32_t app_id{0};
    uint64_t registration_epoch{0};
    uint32_t request_id{0};
    std::string username;
    uint64_t route_token{0};
};
```

#### ClaimUsernameStatus

```cpp
enum class ClaimUsernameStatus : uint8_t
{
    Granted = 0,
    AlreadyClaimed = 1,
    InvalidRoute = 2,
    ExpiredRoute = 3,
    InvalidRegistration = 4,
    CoordinatorUnavailable = 5,
};
```

#### ClaimUsernameResult

```cpp
struct ClaimUsernameResult
{
    uint32_t request_id{0};
    ClaimUsernameStatus status{ClaimUsernameStatus::CoordinatorUnavailable};
    uint64_t claim_lease_id{0};
    uint32_t claim_ttl_ms{0};
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
    uint64_t claim_lease_id{0};
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
    uint64_t claim_lease_id{0};
    ReleaseReason reason{ReleaseReason::Failed};
};
```

## 9. Route token 设计

### 9.1 设计目标

`route_token` 是当前 `CoordinatorIngress` 模式下的短期路由租约。

它的用途不是认证，而是:

- 证明客户端确实经过 `LoginAppMgr` 选路
- 约束该请求只能打到指定 `LoginApp`
- 避免客户端绕过 coordinator 随机命中任意 LoginApp

### 9.2 当前阶段实现

当前阶段使用**状态型 opaque token**，不做无状态签名票据。

建议结构:

```cpp
struct RouteLeaseEntry
{
    uint64_t route_token{0};
    uint32_t app_id{0};
    uint64_t registration_epoch{0};
    Address loginapp_internal_addr;
    Address loginapp_external_addr;
    TimePoint created_at{};
    TimePoint expires_at{};
    bool consumed{false};
};
```

状态索引:

```cpp
std::unordered_map<uint64_t, RouteLeaseEntry> route_leases_;
```

### 9.3 语义约束

- `route_token` 必须是随机不可预测值
- TTL 建议 3 到 5 秒
- 单次消费
- 只能被被分配的目标 `LoginApp` 使用
- 过期后自动回收
- manager 重启后全部失效

### 9.4 为什么不用“返回地址即完成”

如果 `Resolve` 成功后只返回地址，不发 token，会有两个问题:

- 客户端可以绕过 coordinator 直接扫所有 `LoginApp`
- `LoginApp` 无法区分“合法路由进来的请求”和“旁路直连请求”

所以即使当前阶段由 `LoginAppMgr` 做入口，也不能只返回裸地址。

## 10. Claim 设计

### 10.1 设计目标

claim 是全集群用户名互斥的唯一真相来源。

### 10.2 ClaimEntry

```cpp
struct ClaimEntry
{
    uint32_t app_id{0};
    uint64_t registration_epoch{0};
    uint32_t request_id{0};
    uint64_t claim_lease_id{0};
    TimePoint claimed_at{};
    TimePoint expires_at{};
};
```

索引建议:

```cpp
std::unordered_map<std::string, ClaimEntry> claims_by_username_;
std::unordered_map<uint32_t, std::unordered_set<std::string>> usernames_by_app_;
```

### 10.3 语义要求

工业级实现必须满足:

- `ReleaseUsername` 只有在
  `(app_id, registration_epoch, request_id, username, claim_lease_id)` 全匹配时才生效
- `RenewUsernameClaim` 同样要求 owner 全量匹配
- `ClaimUsernameResult` 不能只返回 `bool`
- 任何 claim/release/renew 都必须校验消息来源:
  - `src == registered internal_addr`
  - `channel_id == registered channel_id`
  - `registration_epoch == current session epoch`

推荐初始值:

- `LoginApp` pending timeout: 10s
- claim lease TTL: 30s
- renew 周期: 10s

## 11. 负载聚合与选路

### 11.1 LoginAppInfo

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
    uint32_t pending_route_leases{0};
    bool is_ready{false};
    TimePoint last_load_report_at{};
};
```

### 11.2 选路原则

`ResolveLoginApp` 时按以下优先级选路:

1. 实例必须 `is_ready`
2. 负载上报必须新鲜
3. 优先选择 `effective_load` 最低实例

建议计算:

```cpp
effective_load = measured_load + pending_route_leases * epsilon
```

其中:

- `epsilon` 是很小的预留因子，例如 `0.01`
- 只作为入口分散的轻量提示，不等同于真实 pending login

### 11.3 为什么不在 Resolve 时直接做重负载预留

如果在 `Resolve` 成功时就把这次请求当作真实登录负载，会放大:

- 客户端探测流量
- 超时重试
- 拿到地址但未继续登录的请求

因此:

- `Resolve` 只增加轻量 `pending_route_leases`
- `Claim` 成功后才算进入真实 pending login 压力

## 12. LoginApp 改造要求

### 12.1 新增配置

```text
login_ingress_mode
coordinator_required
require_route_token
```

### 12.2 新增成员

```cpp
ChannelId loginappmgr_channel_id_{kInvalidChannelId};
uint32_t my_app_id_{0};
uint64_t registration_epoch_{0};
TimePoint last_load_report_{};
bool coordinator_required_{true};
bool require_route_token_{true};
```

### 12.3 LoginRequest 扩展

`login::LoginRequest` 新增:

```cpp
uint64_t route_token{0};
```

### 12.4 `on_login_request()` 顺序

`LoginApp` 收到请求后必须按以下顺序处理:

1. 本地 rate limit
2. 本地 pending 上限检查
3. 检查 coordinator 是否健康
4. 如果 `require_route_token == true`，校验客户端请求里必须带 token
5. 检查本地 `pending_by_username_`
6. 发送 `ClaimUsername(username, route_token)`
7. claim 成功后进入 `AuthLogin`

### 12.5 LB 模式兼容

未来切到 `LB` 时:

- `require_route_token = false`
- `LoginRequest.route_token` 可为空
- `ClaimUsername` 仍可复用当前协议
- `LoginAppMgr` 在 `ClaimUsername` 路径中跳过 route 校验，仅保留 username claim

这就是本文要求保留的关键扩展点。

## 13. LoginAppMgr 实现要求

### 13.1 双网络面

`LoginAppMgr` 必须是双网络结构:

```cpp
class LoginAppMgr : public ManagerApp
{
public:
    LoginAppMgr(EventDispatcher&, NetworkInterface& internal_network,
                NetworkInterface& external_network);

private:
    NetworkInterface& external_network_;
};
```

其中:

- `internal_network` 复用 `ManagerApp`
- `external_network_` 只处理 `ResolveLoginApp`

### 13.2 模块拆分

为了未来切到 `LB`，建议在代码结构上拆分:

```text
src/server/loginappmgr/
├── loginappmgr.hpp / .cpp
├── loginappmgr_messages.hpp
├── loginappmgr_ingress.hpp / .cpp
└── main.cpp
```

模块职责:

- `loginappmgr.cpp`
  - register / validate / claim / release / load aggregation
- `loginappmgr_ingress.cpp`
  - external RUDP listener
  - `ResolveLoginApp`
  - route lease 管理

未来切到 `LB` 时，只需 disable `loginappmgr_ingress` 的对外监听，不动核心协调层。

### 13.3 Handler 风格

所有内部 handler 都必须遵循与 `BaseAppMgr` 一致的校验模式:

- 先按 `app_id` 找实例
- 再校验 `src`
- 再校验 `channel_id`
- 再校验 `registration_epoch`

不能像粗略草案那样只信任消息体里的 `app_id`。

## 14. 观测与运维

最少需要暴露以下 watcher / metrics:

- `loginappmgr/loginapp_count`
- `loginappmgr/healthy_loginapp_count`
- `loginappmgr/route_lease_count`
- `loginappmgr/route_resolve_total`
- `loginappmgr/route_resolve_fail_total`
- `loginappmgr/claim_count`
- `loginappmgr/claim_grant_total`
- `loginappmgr/claim_reject_total`
- `loginappmgr/source_validation_fail_total`

每个 `LoginApp` 至少暴露:

- `loginapp/coordinator_connected`
- `loginapp/require_route_token`
- `loginapp/pending_claims`
- `loginapp/claim_timeout_total`
- `loginapp/claim_release_total`

## 15. 当前不做的事情

本阶段不做:

- 客户端可见全局排队
- manager 崩溃后 replay pending claim
- route token 跨重启恢复
- 无状态签名 route token
- active-standby LoginAppMgr

原因很简单:

- 这些都不是“把当前入口方案做对”的必要条件
- 过早加入只会显著提高协议和恢复复杂度

## 16. 实施阶段

按工程优先级调整为 6 个阶段:

| 阶段 | 内容 | 是否进入当前范围 |
|------|------|------------------|
| P1 | `ProcessType` / message IDs / config 开关 | 是 |
| P2 | `loginappmgr_messages.hpp` + 单元测试 | 是 |
| P3 | `LoginAppMgr` 内部协调层: register / validate / claim / renew / release | 是 |
| P4 | `LoginAppMgr` 外部入口层: resolve / route lease / external RUDP | 是 |
| P5 | `LoginApp` 集成: route token + claim-first 状态机 + fail-closed | 是 |
| P6 | `LBIngress` 切换支持与开关验证 | 预留接口，暂不完整实现 |

### 当前交付标准

本期只有满足以下条件才算完成:

- 客户端可以先连 `LoginAppMgr` 并拿到目标 `LoginApp`
- `route_token` 只能被目标 `LoginApp` 单次消费
- 多 `LoginApp` 部署下用户名全局 claim 正确
- manager 不可用时新登录 fail-closed
- `LoginRequest` 协议对未来 `LB` 模式兼容
- 单元测试覆盖 route lease / token 过期 / owner 校验 / epoch 校验
- 集成测试覆盖多实例重复用户名竞争与目标实例下线重试

## 17. 未来切换到 LB 的路径

将来切换到 `LB` 时，按如下步骤演进:

1. 客户端入口改为统一 `LB`
2. `login_ingress_mode` 切到 `lb`
3. `LoginAppMgr` 停止对外监听 `ResolveLoginApp`
4. `require_route_token` 切为 `false`
5. `LoginApp` 保持 claim-first 状态机不变
6. `LoginAppMgr` 保留内部协调职责

换句话说，未来切换时被替换的是**入口选路层**，不是**内部协调层**。

## 18. 变更文件清单

| 操作 | 文件 | 阶段 |
|------|------|------|
| 修改 | `src/lib/server/server_config.hpp` | P1 |
| 修改 | `src/lib/server/server_config.cpp` | P1 |
| 修改 | `src/lib/network/message_ids.hpp` | P1 |
| 修改 | `src/server/CMakeLists.txt` | P3/P4 |
| 新增 | `src/server/loginappmgr/CMakeLists.txt` | P3/P4 |
| 新增 | `src/server/loginappmgr/loginappmgr_messages.hpp` | P2 |
| 新增 | `src/server/loginappmgr/loginappmgr.hpp` | P3/P4 |
| 新增 | `src/server/loginappmgr/loginappmgr.cpp` | P3 |
| 新增 | `src/server/loginappmgr/loginappmgr_ingress.hpp` | P4 |
| 新增 | `src/server/loginappmgr/loginappmgr_ingress.cpp` | P4 |
| 新增 | `src/server/loginappmgr/main.cpp` | P3/P4 |
| 修改 | `src/server/loginapp/login_messages.hpp` | P5 |
| 修改 | `src/server/loginapp/loginapp.hpp` | P5 |
| 修改 | `src/server/loginapp/loginapp.cpp` | P5 |
| 修改 | `tests/unit/` 下 LoginAppMgr 消息与 lease/claim 测试 | P2/P3/P4 |
| 修改 | `tests/integration/` 下多 LoginApp 登录竞争测试 | P5 |

## 19. 部署拓扑示例

### 当前阶段

```text
机器 A: machined + LoginAppMgr(ext+int) + BaseAppMgr + CellAppMgr + DBApp
机器 B: machined + LoginApp-1 + BaseApp-1 + CellApp-1
机器 C: machined + LoginApp-2 + BaseApp-2 + CellApp-2
机器 D: machined + LoginApp-3 + BaseApp-3 + CellApp-3
```

客户端配置只需要知道 `LoginAppMgr` 的统一入口地址。

### 未来阶段

```text
公网入口: SLB / Envoy / Nginx stream / K8s Service
    -> LoginApp-{1..N}.external_port

LoginAppMgr:
    只保留 internal control-plane
```

## 20. 与前一版草案的关键差异

本次修订相对前一版文档的变化:

- 恢复 `LoginAppMgr` 对外入口方案
- 不再强行要求当前阶段必须使用 `LB`
- 引入 `route_token` 作为两段式入口的核心租约机制
- 明确拆分 `Resolve` 与 `Claim`
- 保留未来 `LBIngress` 的协议与代码扩展点
- 明确 `LoginAppMgr` 将长期保留，不因未来引入 `LB` 而消失

保持不变的工业级约束:

- claim/release 必须具备 owner 校验
- manager 重启后不 replay 旧 claim
- 多实例下不允许自动回退本地去重
- 先做正确性，再做 queue 和 HA
