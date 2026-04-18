# Phase 9: LoginApp + BaseAppMgr — 登录流程

> 前置依赖: Phase 8 (BaseApp), Phase 7 (DBApp), Phase 6 (machined)
> BigWorld 参考: `server/loginapp/`, `server/baseappmgr/`, `lib/connection/login_interface.hpp`
> 文档状态: 2026-04-18 当前实现快照

---

## 目标

Phase 9 完成单集群登录主线: `LoginApp` 接受客户端登录，`DBApp` 认证，`BaseAppMgr` 分配 `BaseApp`，`BaseApp` 完成 `PrepareLogin` 与最终 `Authenticate`。

多 `LoginApp` 与 `LoginAppMgr` 的扩展设计见 [../MULTI_LOGINAPP_DESIGN.md](../MULTI_LOGINAPP_DESIGN.md)。

---

## 总体状态

**功能主线: ✅ 已完成 / 稳定性压测: 🚧 收敛中**

| 模块 | 状态 | 说明 |
|------|------|------|
| 入口与传输层 (外部 RUDP) | ✅ | 双 `NetworkInterface`，客户端短 inactivity timeout |
| 登录主链路 (`AuthLogin -> AllocateBaseApp -> PrepareLogin -> Authenticate`) | ✅ | 九步时序已全部落地 |
| `SessionKey` 机制 | ✅ | 32 字节随机值，客户端凭此认领 BaseApp |
| 速率限制 (per-IP + global + trusted CIDR bypass) | ✅ | |
| 重复登录 (本地快路径 + 跨 BaseApp `ForceLogoff`) | ✅ | |
| `BaseAppMgr` 注册 / EntityID 区间分配 / 负载上报 | ✅ | |
| 基于扩展负载和 `dbid affinity` 的 BaseApp 选择 | ✅ | |
| BaseApp 过载保护并反馈到分配决策 | ✅ | |
| Global Bases 注册 / 注销 / 广播 | ✅ | |
| `PrepareLogin` 可选 `entity_blob` 预取路径 | ✅ | |
| `CancelPrepareLogin` 回滚路径 | ✅ | |
| 定向单元测试 + 模块级集成测试 | ✅ | 见下文测试清单 |
| 极端短线重登 churn 压测收敛 | 🚧 | 仍有 `timeout_fail` / `invalid_session` / `no_dbapp` / checkout 长尾残余 |
| 端到端长稳 / 长链路量化 | 🚧 | `force-logoff -> relogin -> checkout -> authenticate` 需继续量化 |

残余问题详情见:

- [../LOGIN_STRESS_REMAINING_ISSUES_20260412.md](../LOGIN_STRESS_REMAINING_ISSUES_20260412.md)
- [../LOGIN_ROLLBACK_PROTOCOL_20260412.md](../LOGIN_ROLLBACK_PROTOCOL_20260412.md)

---

## 已落地实现摘要 ✅

- **登录主链路九步时序**: Client → LoginApp → DBApp → BaseAppMgr → BaseApp → Client → BaseApp，已全部实现
- **协议消息** (ID 5000–5008): `LoginRequest` / `LoginResult` / `AuthLogin` / `AuthLoginResult` / `AllocateBaseApp` / `AllocateBaseAppResult` / `PrepareLogin` / `PrepareLoginResult` / `CancelPrepareLogin`
- **`LoginStatus` 状态码**: 包含 `Success` / `InvalidCredentials` / `AlreadyLoggedIn` / `ServerFull` / `RateLimited` / `ServerNotReady` / `InternalError` / `LoginInProgress` / `ServerBusy`
- **`PrepareLogin` 字段**: `request_id` / `type_id` / `dbid` / `session_key` / `client_addr` / `blob_prefetched` / `entity_blob`
- **`BaseAppMgr::InformLoad` 扩展字段**: `load` / `entity_count` / `proxy_count` / `pending_prepare_count` / `pending_force_logoff_count` / `detached_proxy_count` / `logoff_in_flight_count` / `deferred_login_count`
- **`BaseAppMgr` 分配策略**: 综合 `measured_load` / `effective_load` / queue pressure / `dbid affinity` / overload gate
- **`BaseApp` 登录侧复杂度**: checkout 冲突处理、detached proxy grace、本地重登快路径、远端 `ForceLogoff` + `ForceLogoffAck` 重试、deferred checkout / retry、`CancelPrepareLogin` 回滚

---

## 当前模块职责 ✅

| 模块 | 主要职责 |
|------|----------|
| `LoginApp` | 接受客户端 RUDP、速率限制、用户名去重、驱动主链路、超时与 abandoned login 清理 |
| `BaseAppMgr` | BaseApp 注册、`app_id` / `EntityID` 区间分配、扩展负载跟踪、带 affinity 的分配、过载保护、Global Bases |
| `BaseApp` | `PrepareLogin` 接收、checkout 冲突收敛、detached proxy grace、prepared/pending/deferred login 管理、`Authenticate(session_key)` 认领 |
| `DBApp` | `AuthLogin` 认证、`auto_create_accounts`、checkout / checkin / abort 协调 |

---

## 当前测试产物 ✅

与 Phase 9 直接相关的测试:

**单元测试**:
- `tests/unit/test_login_messages.cpp`
- `tests/unit/test_login_rollback.cpp`
- `tests/unit/test_baseappmgr_messages.cpp`
- `tests/unit/test_checkout_manager.cpp`

**集成测试**:
- `tests/integration/test_baseappmgr_registration.cpp`
- `tests/integration/test_login_flow.cpp`
- `tests/integration/test_dbapp_login_flow.cpp`
- `tests/integration/test_dbapp_checkout_cleanup.cpp`

定向单元测试与模块级集成测试已具备继续推进后续阶段的条件。

---

## 稳定性收敛 — 后续高优先级方向 🚧

截至 2026-04-18，主要残余问题是**极端 churn 下跨进程生命周期收敛**，不是功能缺失。

1. 把 `force-logoff -> relogin -> checkout -> authenticate` 的长链路继续量化
2. 核查 `BaseApp` 本地状态机是否存在未清理或推进顺序竞争
3. 核查 `DBApp` 在多 BaseApp 压力下的排队与回复长尾
4. 补完整端到端行为测试，而不是继续只靠压测日志猜测

压测方法与参数见 [../LOGIN_STRESS_TESTING.md](../LOGIN_STRESS_TESTING.md)。
详细问题分解见 [../LOGIN_STRESS_REMAINING_ISSUES_20260412.md](../LOGIN_STRESS_REMAINING_ISSUES_20260412.md)。

---

## 与其他文档的边界

**本文档负责**:

- 单 `LoginApp` 基线登录链路
- 当前 `LoginApp` / `BaseAppMgr` / `BaseApp` / `DBApp` 在登录链路上的真实实现状态
- 已落地功能、已知问题和剩余验证项

**本文档不再负责** (统一见 [../MULTI_LOGINAPP_DESIGN.md](../MULTI_LOGINAPP_DESIGN.md)):

- 多 `LoginApp` 入口与 `LoginAppMgr` 外部暴露方案
- `route_token`
- `LoginAppMgr` ingress / coordinator 双模式切换
- 全局排队系统

---

## 当前结论

- Phase 9 单实例登录主线 **功能已完成** ✅
- `BaseAppMgr` 已从基础分配器演进到带扩展负载语义和 affinity 的分配器
- `BaseApp` 已承载重复登录、回滚和准备阶段的大部分复杂性
- 系统已可推进后续多实例设计
- 极端短线重登压力下跨进程生命周期收敛**仍在收敛** 🚧
