# Phase 13: 高可用 — Reviver + DBAppMgr

**Status:** 🚧 部分落地。CellAppMgr 本地 Snapshot / Restore 与最小 Reviver
已可用；DBAppMgr / BaseAppMgr 高可用尚未启动。
**前置依赖:** Phase 11（分布式空间完整可用）
**BigWorld 参考:** `server/reviver/`, `server/dbappmgr/`

## 目标

为 Atlas 集群添加故障恢复和高可用能力，确保单个进程崩溃不会导致整个
集群不可用。

## 关键设计决策

### Reviver 进程

BigWorld 的 Reviver 只负责监督全局唯一 Manager 进程；Manager 崩溃后的
状态恢复由对应 Manager 自己的 Snapshot / Restore 完成。普通进程
（BaseApp / CellApp / DBApp）的故障由对应 Manager 处理。

当前 Atlas 已落地 CellAppMgr 的最小 Reviver：订阅 machined birth / death，
按进程名识别目标 CellAppMgr，异常退出后延迟重启，并可在启动时 cold-start
缺失的 CellAppMgr。Reviver 会把固定 internal port、snapshot path 和
update hertz 传给新 CellAppMgr，使重启进程能从本地快照恢复并重新注册。

仍缺：BaseAppMgr / DBAppMgr 监控、Reviver 多实例选举、独立心跳超时检测。
当前异常检测依赖 machined death 通知；最大重启次数默认 3。

### Manager 状态备份与恢复

Manager 进程定期备份关键状态，崩溃重启后可恢复。目标统一接口：

```
IManagerBackup:
  Snapshot()  -> bytes
  Restore(bytes) -> Result<void>
```

各 Manager 备份内容：

| Manager | 备份内容 |
|---|---|
| BaseAppMgr | BaseApp 列表、负载数据、全局实体 ID 分配器 |
| CellAppMgr | Space 分区信息（BSP 树）、CellApp 列表 |
| DBAppMgr | DBApp 列表、待处理请求队列 |

当前 CellAppMgr 已实现 `Snapshot()` / `Restore()`，并支持 `--snapshot-path`
周期写本地快照；启动时会先尝试读取快照。快照内容包括 CellApp 列表、
`next_app_id`、Space BSP、pending geometry broadcasts、global geometry version
和 tick alignment epoch。恢复出的 CellApp 标记为待 reattach；真实 CellApp
重新注册后，CellAppMgr 会 replay topology 并请求 CellApp 主动上报最新负载。

仍缺：BaseAppMgr / DBAppMgr Snapshot / Restore，以及快照过期后从集群重建
Manager 状态。

### DBAppMgr — 多 DBApp 实例

管理多 DBApp + 提供故障转移。

**分片策略：**

- 简单：按 `dbid % num_dbapps`
- 高级：按实体类型分片（不同类型路由到不同 DBApp）
- 故障转移：DBApp 挂掉后，其负责的分片迁移到其他 DBApp

### CellApp 故障恢复

**方案 A — 重新分配 Cell：** Cell 分配给其他 CellApp；其他 CellApp 上的
Ghost 升级为 Real；无 Ghost 的实体从 BaseApp 重建。

**方案 B — 重启 CellApp：** 从 BaseApp 获取实体列表；重新创建所有实体；
重建 Ghost 关系。

### BaseApp 故障恢复

客户端检测断线后重新登录（走 LoginApp 流程）。Base 实体从 DBApp 重新
加载（最近一次 `WriteToDB` 的状态）；Cell 实体保持不变（在 CellApp 上）。

**数据丢失窗口：** 上次 `WriteToDB` 到崩溃之间的状态变更会丢失。
**优化：** 定期自动 `WriteToDB`（每 N 分钟自动持久化所有 Proxy 实体）
缩小窗口。

## 验收标准

- 已满足：最小 Reviver 可 cold-start CellAppMgr，并在异常终止后重启真实
  CellAppMgr 进程
- 已满足：CellAppMgr 可从本地 snapshot 文件恢复 BSP 拓扑和 CellApp 表
- 待补齐：Reviver 检测 BaseAppMgr / DBAppMgr 崩溃并自动重启
- DBAppMgr 支持多 DBApp 实例和故障转移
- CellApp 崩溃后管理的实体可迁移到其他 CellApp
- BaseApp 崩溃后客户端可重连到新 BaseApp

## 待细化问题

- Reviver 多实例选举算法
- Manager 状态备份的存储位置（本地文件 vs 共享存储）
- CellApp 恢复期间的消息缓冲
- 客户端重连时的状态恢复（是否需要发送完整实体状态）
- 数据丢失窗口可接受范围（影响自动 `WriteToDB` 频率）
- 是否引入 WAL（Write-Ahead Log）减少数据丢失
