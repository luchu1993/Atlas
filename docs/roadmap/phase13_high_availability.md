# Phase 13: 高可用 — Reviver + DBAppMgr

**Status:** ⬜ 未启动。本文是设计文档。
**前置依赖:** Phase 11（分布式空间完整可用）
**BigWorld 参考:** `server/reviver/`, `server/dbappmgr/`

## 目标

为 Atlas 集群添加故障恢复和高可用能力，确保单个进程崩溃不会导致整个
集群不可用。

## 关键设计决策

### Reviver 进程

监控全局唯一的 Manager 进程（BaseAppMgr / CellAppMgr / DBAppMgr），
心跳超时后自动重启。普通进程（BaseApp / CellApp / DBApp）的故障由
对应 Manager 处理。Reviver 自身的高可用：可部署多个 Reviver，通过选举
确定 active。

策略：心跳超时阈值 ≈ 5s；最大重启次数 3。

### Manager 状态备份与恢复

Manager 进程定期备份关键状态，崩溃重启后可恢复。统一接口：

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

策略：定期快照到本地文件（每 N 秒）；Manager 重启后先尝试读取最新
快照；快照过期时从集群中各进程重建状态。

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

- Reviver 检测 Manager 崩溃并自动重启
- Manager 重启后从备份恢复状态
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
