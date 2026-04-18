# Phase 8: BaseApp — 实体宿主与客户端代理

> 前置依赖: Phase 7 (DBApp), Phase 5 (ScriptApp/EntityApp), Script Phase 4 最小实体/RPC 子集
> BigWorld 参考: `server/baseapp/baseapp.hpp`, `server/baseapp/base.hpp`, `server/baseapp/proxy.hpp`
> 当前代码基线 (2026-04-18): Phase 8 主体已完成。BaseApp 具备双网络接口、实体宿主、登录准备/认证/ForceLogoff、DB 持久化与恢复、客户端转移、属性增量转发、基线快照、以及完整的 C# 回调表 (`RestoreEntity` / `GetEntityData` / `EntityDestroyed` / `DispatchRpc` / `GetOwnerSnapshot`)。外部接口已落地 `Authenticate` / `AuthenticateResult` / `ClientBaseRpc`；`ClientCellRpc`、`EnableEntities`、`HeartbeatPing` 以及 AOI 多观察者 fan-out 仍由 Phase 10 / Phase 12 承接。

---

## 目标

实现 Atlas 的**第一个真正的游戏服务器进程**。BaseApp 持有实体的 Base 部分，
管理客户端连接，处理 RPC 路由和数据库持久化。

## 验收标准

- [x] BaseApp 可启动，注册到 machined / BaseAppMgr，初始化 C# 脚本引擎
- [x] 可创建 Base 实体，C# 脚本逻辑 (`OnInit`, `OnTick`) 可执行
- [x] 可创建 Proxy 实体，接受客户端 RUDP 连接
- [x] 客户端 exposed base RPC 调用经安全校验后分发到 C# 实体 (`ClientBaseRpc` + `EntityDefRegistry::FindRpc`)
- [x] C# 实体 client_methods 调用经 C++ 路由发送到客户端 (`SendClientRpc`)
- [x] `WriteToDB()` 将 persistent 属性持久化到 DBApp
- [x] `CreateEntityFromDB()` 从 DBApp 加载实体并恢复 C# 状态 (`RestoreManagedEntity`)
- [x] `GiveClientTo()` 本地 + 远程 (跨 BaseApp) 转移客户端连接
- [x] 可复制属性增量同步到 Proxy 的客户端 (`DeltaForwarder` + baseline snapshot)
- [x] 全部新增代码有单元测试

## 验收状态 (2026-04-18)

- ✅ 主体完成：实体宿主 + 登录准备/认证 + DB 往返 + 本地/远程客户端转移 + 属性增量转发 + 基线快照
- ⬜ 延后到 Phase 10 / Phase 12：`ClientCellRpc`、`EnableEntities`、`HeartbeatPing`、AOI 多观察者 fan-out

---

## 1. BigWorld 架构分析与 Atlas 适配 ✅

- BaseApp 采用 `BaseApp : EntityApp : ScriptApp : ServerApp` 层次，双网络接口 (内部 / 外部)，实体 ID 由 `IDClient` 从 DBApp 批量获取。
- Base/Proxy 薄壳：C++ 只做网络 I/O、消息路由、安全校验、生命周期；属性管理、序列化、RPC 分发全部由 C# Source Generator 生成。
- `restore_entity` / `get_entity_data` / `entity_destroyed` / `dispatch_rpc` / `get_owner_snapshot` 五个回调构成 C++ → C# 调用通道。

---

## 2. 消息协议设计 ✅

### 2.1 BaseApp 内部接口 (从其他服务器进程)

| 消息 | ID | 方向 | 状态 |
|------|-----|------|------|
| `CreateBase` | 2000 | BaseAppMgr → BaseApp | ✅ |
| `CreateBaseFromDB` | 2001 | BaseAppMgr → BaseApp | ✅ |
| `AcceptClient` | 2002 | BaseApp → BaseApp | ✅ |
| `CellEntityCreated` | 2010 | CellApp → BaseApp | ✅ |
| `CellEntityDestroyed` | 2011 | CellApp → BaseApp | ✅ |
| `CurrentCell` | 2012 | CellApp → BaseApp | ✅ |
| `CellRpcForward` | 2013 | CellApp → BaseApp | ✅ |
| `SelfRpcFromCell` | 2014 | CellApp → BaseApp | ✅ |
| `ReplicatedDeltaFromCell` | 2015 | CellApp → BaseApp | ✅ |
| `BroadcastRpcFromCell` | 2016 | CellApp → BaseApp | ✅ |
| `ReplicatedReliableDeltaFromCell` | 2017 | CellApp → BaseApp | ✅ |
| `ForceLogoff` / `ForceLogoffAck` | 2030 / 2031 | BaseApp ↔ BaseApp | ✅ |
| `WriteEntityAck` | (dbapp) | DBApp → BaseApp | ✅ |

### 2.2 BaseApp 外部接口 (从客户端)

| 消息 | ID | 方向 | 状态 |
|------|-----|------|------|
| `Authenticate` | 2020 | Client → BaseApp | ✅ |
| `AuthenticateResult` | 2021 | BaseApp → Client | ✅ |
| `ClientBaseRpc` | 2022 | Client → BaseApp | ✅ |
| `ReplicatedBaselineToClient` | 0xF002 | BaseApp → Client | ✅ |
| `kClientDeltaMessageId` | 0xF001 | BaseApp → Client | ✅ (DeltaForwarder) |
| `kClientReliableDeltaMessageId` | 0xF003 | BaseApp → Client | ✅ |

### 2.3 尚未落地的外部协议 ⬜

| 消息 | 规划方向 | 承接阶段 |
|------|----------|----------|
| `ClientCellRpc` | Client → BaseApp → CellApp | Phase 10 |
| `EnableEntities` | Client → BaseApp | Phase 12 |
| `HeartbeatPing` | Client → BaseApp | Phase 12 / 后续 |
| `EntityEnter` / `EntityLeave` | BaseApp → Client (AOI) | Phase 10 |
| `EntityPropertyUpdate` (观察者) | BaseApp → Client | Phase 10 |
| `CreateBasePlayer` / `ResetEntities` | BaseApp → Client | Phase 12 SDK |

> `kClientCellRpc = 2023` 已在 `message_ids.h` 预留 ID，但 `struct ClientCellRpc` 仍待 Phase 10 定义。

---

## 3. 核心模块设计 ✅

### 3.1 BaseEntity / Proxy ✅

`src/server/baseapp/base_entity.h`+`.cc` 实现：
- `BaseEntity` 持有 entity_id / type_id / dbid / cell 关联 / 持久化 blob
- `Proxy : BaseEntity` 持有 client_addr + SessionKey + session_epoch + detached_grace
- `MarkForDestroy` / `OnWriteAck` 生命周期

### 3.2 EntityManager ✅

`src/server/baseapp/entity_manager.h`+`.cc` 实现：
- `IDClient` 注入、`AllocateId` / `IsRangeLow`
- 主表 + dbid 索引 + session_key 索引 + retired_session_keys (5s TTL)
- `FindProxyBySession` 支持登录时的短时会话重叠
- `AssignDbid` / `AssignSessionKey` / `ClearSessionKey` 维护二级索引

### 3.3 BaseAppNativeProvider ✅

`src/server/baseapp/baseapp_native_provider.h`+`.cc` 实现全部 `INativeApiProvider` 钩子：
- `SendClientRpc` / `SendCellRpc` / `SendBaseRpc`
- `WriteToDb`
- `GiveClientTo`
- `SetNativeCallbacks` (解析打包的 `NativeCallbackTable`)
- 五个 C++ → C# 回调: `RestoreEntityFn`, `GetEntityDataFn`, `EntityDestroyedFn`, `DispatchRpcFn`, `GetOwnerSnapshotFn`

### 3.4 DeltaForwarder ✅

`src/server/baseapp/delta_forwarder.h`+`.cc` 每客户端字节预算 (16 KB/tick) 的 delta 转发队列；相同 entity 的新 delta 替换旧 delta，deferred_ticks 递增防止饥饿。配套 `ReplicatedReliableDeltaFromCell` 绕过预算，以及 `kBaselineInterval = 30 ticks` 的可靠全量快照。

### 3.5 IDClient ✅

`src/server/baseapp/id_client.h`+`.cc` + DBApp `GetEntityIds` 水位请求。`MaybeRequestMoreIds()` 在 tick 结束时检查低水位。

### 3.6 BaseApp 主进程 ✅

`src/server/baseapp/baseapp.h`+`.cc` (2200+ 行) 完整实现：
- `Init` / `Fini` / `OnEndOfTick` / `OnTickComplete` / `RegisterWatchers`
- 双网络接口 (`internal_network` + `external_network`)
- 完整登录流水线：`PrepareLogin` → checkout / ForceLogoff 重试 → blob prefetch → `AuthenticateResult`
- `ForceLogoff` / `ForceLogoffAck` 远程驱逐 + 指数退避
- 检出冲突、取消、回滚 (`RollbackPreparedLoginEntity`)
- 延迟登录队列 (`deferred_login_checkouts_`)、detached proxy grace 窗口
- 快速重登录与跨 BaseApp 重登录 (`TryCompleteLocalRelogin`, `RotateProxySession`)
- DB 往返 (`DoWriteToDb`, `BeginLogoffPersist`, `BeginForceLogoffPersist`)
- 本地 + 远程 `GiveClientTo` (`DoGiveClientToLocal`, `DoGiveClientToRemote`)
- 负载上报 (`LoadTracker` + `InformLoad`)
- Watcher 指标：delta_bytes_sent、baseline_messages、auth_success/fail、force_logoff、fast_relogin 等

### 3.7 外部接口安全 ✅

实际策略：`ch.RemoteAddress()` 与 Proxy 绑定、`FindProxyBySession` 会话密钥校验、认证前仅允许 `Authenticate`、`EntityDefRegistry::FindRpc` 校验 exposed scope。认证超时/速率限制复用 `NetworkInterface` 能力。

---

## 4. 端到端流程 ✅

下列流程均已在代码中实现并有测试覆盖：
- 创建实体 (C# `EntityFactory.Create` → `NativeApi` → `EntityManager::Create`)
- 客户端 exposed base RPC (`OnClientBaseRpc` → `FindRpc` → `dispatch_rpc_fn`)
- `WriteToDB` (`DoWriteToDb` → `WriteEntity` → `OnWriteAck`)
- `GiveClientTo` 本地 + 远程 (`DoGiveClientToLocal` / `DoGiveClientToRemote` + `AcceptClient`)
- `CreateEntityFromDB` (`CheckoutEntity` → blob prefetch → `RestoreManagedEntity` → `restore_entity_fn`)
- 属性增量同步 (`ReplicatedDeltaFromCell` → `DeltaForwarder::Enqueue` → `FlushClientDeltas`)
- 基线快照 (`EmitBaselineSnapshots` → `GetOwnerSnapshotFn` → `ReplicatedBaselineToClient`)

---

## 5. 实现步骤 (回顾)

| Step | 说明 | 状态 |
|------|------|------|
| 8.1  | BaseApp 消息定义 (`baseapp_messages.h`) | ✅ |
| 8.2  | BaseEntity | ✅ |
| 8.3  | Proxy (合并在 `base_entity.h`) | ✅ |
| 8.4  | EntityManager | ✅ |
| 8.5  | BaseAppNativeProvider | ✅ |
| 8.6  | BaseApp 进程主体 + 登录流水线 + DB 集成 | ✅ |
| 8.7  | `INativeApiProvider` 扩展 + C# 回调表 | ✅ |
| 8.8  | 集成测试骨架 (`test_baseappmgr_registration.cpp`) | ✅ |

Phase 10 / 12 接续：`ClientCellRpc` struct、`EnableEntities`、`HeartbeatPing`、AOI 多观察者 fan-out (当前 `SelfRpcFromCell`/`ReplicatedDeltaFromCell`/`BroadcastRpcFromCell` 仅路由到 `base_entity_id` 对应 Proxy)。

---

## 6. 文件清单 ✅

```
src/server/baseapp/
├── CMakeLists.txt
├── main.cc
├── baseapp.h / .cc
├── baseapp_messages.h
├── base_entity.h / .cc                 (BaseEntity + Proxy)
├── entity_manager.h / .cc
├── baseapp_native_provider.h / .cc
├── delta_forwarder.h / .cc
└── id_client.h / .cc

tests/unit/
├── test_baseapp_messages.cpp
├── test_base_entity.cpp
├── test_entity_manager.cpp
├── test_delta_forwarder.cpp
├── test_id_client.cpp
├── test_login_rollback.cpp
├── test_native_api_exports.cpp
└── test_native_api_provider.cpp

tests/integration/
└── test_baseappmgr_registration.cpp
```

---

## 7. 关键设计决策记录 ✅

- **双网络接口**: 外部 (客户端 RUDP) + 内部 (服务器 RUDP)，安全隔离。
- **C# 实体生命周期**: `uint32_t entity_id` 作为 C++ ↔ C# 纽带；`entity_destroyed_fn` 通知 C# 释放 GCHandle。
- **客户端到实体映射**: `client_entity_index_` (Address → EntityID) + `entity_client_index_` (反向)，断线回调 `OnExternalClientDisconnect` 按 Address 查找。
- **异步销毁**: `MarkForDestroy` + `WriteEntityAck` 回调驱动真正释放；`writing_to_db_` 阻止重入。
- **脏属性同步**: C# 主动推送 delta blob 到 `NativeApi`，C++ 按字节预算转发；reliable 属性走独立通道；kBaselineInterval 定期发送 owner-scope 全量快照兜底 UDP 丢包。
- **登录鲁棒性**: `PrepareLogin` → checkout → (冲突时) `ForceLogoff` + 指数退避 → blob prefetch → `AuthenticateResult`；支持取消、回滚、detached grace、快速重登录。

### 7.1 初期不实现的 BigWorld 功能

| 功能 | 承接阶段 |
|------|---------|
| 客户端 `ClientCellRpc` 转发到 Cell | Phase 10 |
| AOI / 多观察者 fan-out (otherClients/allClients) | Phase 10 |
| `EnableEntities` / `CreateBasePlayer` / `ResetEntities` | Phase 12 SDK |
| 实体备份 (BackupHash) 多 BaseApp 冗余 | Phase 13 |
| DataDownloads 流式下载 | 按需 |
| Global Bases 广播 | Phase 9/13 |
| 客户端加密 / TLS | 按需 |
