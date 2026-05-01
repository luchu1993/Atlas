# Phase 8: BaseApp — 实体宿主与客户端代理

**Status:** ✅ 完成。`ClientCellRpc` 与 AOI 多观察者 fan-out 已在 Phase 10
落地；`EnableEntities` / `HeartbeatPing` 仍由 Phase 12 承接。
**前置依赖:** Phase 7 (DBApp)、Phase 5 (ScriptApp/EntityApp)、Script Phase 4
最小实体 / RPC 子集
**BigWorld 参考:** `server/baseapp/baseapp.hpp`, `base.hpp`, `proxy.hpp`

## 目标

Atlas 的第一个真正的游戏服务器进程。BaseApp 持有实体的 Base 部分，
管理客户端连接，处理 RPC 路由和数据库持久化。

## 类层次与边界

```
BaseApp : EntityApp : ScriptApp : ServerApp
```

- **双网络接口** — 内部 RUDP（服务器集群） + 外部 RUDP（客户端），
  安全隔离。
- **实体 ID 区间** — 由 `IDClient` 从 DBApp 批量预取（`GetEntityIds`
  水位请求，`MaybeRequestMoreIds()` 在 tick 末检查低水位）。
- **C++ 薄壳 + C# 实例** — C++ 只做网络 I/O / 消息路由 / 安全校验 /
  生命周期；属性管理、序列化、RPC 分发由 C# Source Generator 生成。
- **C++ → C# 五个回调通道** — `RestoreEntity` / `GetEntityData` /
  `EntityDestroyed` / `DispatchRpc` / `GetOwnerSnapshot`。

## 关键设计决策

- **客户端到实体映射** — 双索引：`client_entity_index_` (Address →
  EntityID) + `entity_client_index_`（反向）；断线回调
  `OnExternalClientDisconnect` 按 Address 查找。
- **异步销毁** — `MarkForDestroy` + `WriteEntityAck` 回调驱动真正释放；
  `writing_to_db_` 阻止重入。
- **属性同步** — C# 主动推送 delta blob 到 NativeApi，C++ 按字节预算转发
  （`DeltaForwarder`，16 KB/tick）；reliable 属性走独立通道；
  `kBaselineInterval = 30 ticks` 定期发送 owner-scope 全量快照兜底 UDP 丢包。
  同实体新 delta 替换旧 delta，`deferred_ticks` 递增防饥饿。
- **登录鲁棒性** — `PrepareLogin` → checkout → (冲突时) `ForceLogoff` +
  指数退避 → blob prefetch → `AuthenticateResult`；支持取消、回滚、
  detached grace、本地快速重登录、跨 BaseApp 重登录。
- **外部接口安全** — `ch.RemoteAddress()` 与 Proxy 绑定；
  `FindProxyBySession` 校验 session_key；认证前仅允许 `Authenticate`；
  `EntityDefRegistry::FindRpc` 校验 exposed scope。

### 由后续 Phase 承接

| 功能 | 承接阶段 |
|---|---|
| `EnableEntities` / `CreateBasePlayer` / `ResetEntities` | Phase 12 SDK |
| 实体备份（BackupHash）多 BaseApp 冗余 | Phase 13 |
| DataDownloads / Global Bases 广播 / 客户端 TLS | 按需 |
