# Phase 15: DBAppMgr — 多 DBApp 分片管理 + HA

**Status:** 🚧 P15.1 核心中 — DBAppMgr 进程骨架、DBApp 注册、range shard
table、GetShardTable / ShardTableUpdate、watcher、DBApp 注册接入和单元测试
已起步；BaseApp / LoginApp shard cache、DBApp request version / retry
idempotency 与 P15.2 HA 未完成。
**Prereq:** Phase 7(DBApp + DB 层),Phase 13(Manager HA 框架已就位)
**BigWorld 参考:** `server/dbmgr/`

## 0. 为什么

当前 Atlas 只有单 DBApp:

- BaseApp / LoginApp 通过 machined registry 找到唯一一台 DBApp 直接发
  `CheckoutEntity` / `WriteEntity`。
- DBApp 持有 `database_`(SQLite/MySQL backend)、`id_allocator_`、
  `checkout_mgr_`、`pending_checkout_requests_` —— 全是单点。
- 单 DBApp 写吞吐 / 容量 / 故障域都是单点。
- `src/server/dbappmgr/CMakeLists.txt` 只是占位空文件。

DBAppMgr 在 BigWorld 里负责:
1. **多 DBApp 注册表** — 多台 DBApp 各自服务一段 dbid 范围。
2. **客户端路由查询** — BaseApp / LoginApp 问 "dbid X 在哪个 DBApp"。
3. **分片策略** — 一致性哈希 / range 分区,决定新 dbid 分配到哪台。
4. **故障转移** — DBApp 死亡后,接管它的 shard。
5. **Pending request 恢复** — 死亡前在途的 checkout 请求重新路由。

整套和 CellAppMgr / BaseAppMgr 同构,共享 Phase 13 的 Reviver /
worker 重建恢复框架。

## 1. 范围

本 RFC 把 DBAppMgr 拆成两个阶段:

- **P15.1 — DBAppMgr 本体(无 HA)**:实现 mgr 进程 + DBApp 注册 + 路由
  查询 + 简单分片。让客户端 query "dbid → DBApp",但 mgr 自身 crash
  仍丢全部状态。
- **P15.2 — DBAppMgr HA**:加 worker 重建恢复,接入 Reviver
  multi-target 框架和 verify 脚本。镜像 Phase 13 的 CellAppMgr /
  BaseAppMgr 模式,不引入 manager snapshot。

本文档定义 P15.1 + P15.2 的目标设计,并记录当前实现状态。

## 2. 数据流变化(对比当前)

当前路径:

```
LoginApp / BaseApp  ──── machined query DBApp ────→ DBApp
       │                                              │
       └──────── CheckoutEntity / WriteEntity ────────┘
```

P15 之后:

```
LoginApp / BaseApp  ────────→ DBAppMgr  ────route────→ DBApp_shard_K
       │                          ▲                       │
       │     (cached shard map)   │                       │
       │                          │                       ▼
       └── CheckoutEntity / WriteEntity (direct to chosen DBApp)
```

- 启动期客户端向 DBAppMgr 拉取 shard table(dbid range → DBApp addr)。
- 缓存 shard table,checkout/write 直发 DBApp。
- Shard 变更(DBApp 死亡 / 上线)时 DBAppMgr 广播 invalidate;客户端
  重新拉。

## 3. 设计决策(canonical 选择)

### 3.1 分片策略:范围(range)而非一致性哈希

**为什么**:DBApp 持久化数据在磁盘上,搬迁(rebalance)成本高;range
分片让"shard 迁移"等价于"dbid 范围交接",可以 lazy 做 — DBApp_A 把
某个范围的 row 复制到 DBApp_B 完成前,这段范围的请求仍打到 A。一致性
哈希在 DBApp 增减时会 reshuffle 大量 key,不适合带状态的 DB shard。

**实现**:dbid 是 64 位整数。shard 表是 `[low, high) → DBApp` 的有序
区间列表。新 dbid 分配按当前 shard 分布的"最空 DBApp"挑选(类似
BaseAppMgr 的 `FindLeastLoaded`)。

### 3.2 DBApp 注册:走 DBAppMgr 而非 machined

新增 `RegisterDbApp` 消息(DBApp → DBAppMgr),DBApp 启动后**先**
注册到 DBAppMgr,DBAppMgr 在 ack 里返回它的 `dbapp_id` 和初始 shard
范围。machined registry 仍保留 DBApp 的存在,但路由权威在 DBAppMgr。

`dbapp_id` 编码进 dbid 的高 16 位(类似 cellapp app_id 编码在 EntityID
高位),这样客户端拿到一个 dbid 立刻知道默认 owner;DBAppMgr 只在
shard rebalance 时偏离这个 owner。

### 3.3 客户端 shard table 一致性:lease + invalidate

客户端(BaseApp / LoginApp)的 shard table 是缓存,生命周期由 DBAppMgr
租约 (`shard_table_version`) 控制:

- 客户端首次连 DBAppMgr,发 `GetShardTable(known_version=0)`,拿到
  完整表 + 版本号 V。
- 之后每次客户端发 checkout/write 给 DBApp,DBApp 检查请求里携带的
  `shard_table_version`:不匹配则回 `InvalidShardTable(current=V')`,
  客户端重新拉表。
- DBAppMgr 也可以主动 broadcast `ShardTableUpdate(new_version, deltas)`,
  按 BaseAppMgr / CellAppMgr 已有的 broadcast 模式实现。

### 3.4 Pending request 恢复:DBApp 自己负责 retry,不是 DBAppMgr

**为什么**:DBApp 当前已有 `pending_checkout_requests_` 状态,它知道
哪些 request 还没回执;DBApp crash → 客户端通过 machined death
notification 知道 → 客户端把死掉 DBApp 的 in-flight request 重新发给
新 owner DBApp。DBAppMgr 不需要 mirror 这些状态。

**取舍**:简化 DBAppMgr 设计,代价是客户端要在死亡通知后做
de-duplicate(如果 DBApp 已经写了但 ack 丢了,客户端重发会写两次)。
解决:用 `request_id` + DBApp 端 idempotency cache(checkout/write
already-applied 检测)。

### 3.5 DBAppMgr 自身的 HA:走 Phase 13 worker-重建框架

镜像 CellAppMgr / BaseAppMgr 的 BigWorld 式 worker-重建恢复(Phase 13 M1+M2):
DBAppMgr 是软状态,**不持久化 snapshot**——崩溃后由 Reviver 重启,新进程在
recovery 窗口内等存活 DBApp 重注册并重报权威状态,从 worker 报告重建 DBApp 表
和 shard table。

| 步骤 | 内容 |
|---|---|
| P15.2-S1 | DBApp `RecoverDBAppState`(重报 shard ranges)+ recovery 窗口冻结 shard 迁移 |
| P15.2-S2 | `RegisterDBApp.known_app_id` echo 保留 dbapp_id |
| P15.2-S3 | Reviver 多 target 加 dbappmgr |
| P15.2-S4 | verify_dbappmgr_ha.py + docs |

worker 重报的权威状态:
- DBApp 表(addr, dbapp_id, shard ranges, is_retiring, last_load_at)——由重注册
  + `RecoverDBAppState` 重建
- Shard table 版本号(用于客户端缓存失效)——取存活 DBApp 报告的最高版本
- pending shard migration——由 DBApp 重报续做(无 snapshot 续航)

`next_dbapp_id_` 从重报的 dbapp_id 推回(取 max + 1),`known_app_id` 保留原 id。

Reviver 扩 multi-target(已就位的 `ManagedTarget` 框架,加一个
`dbappmgr_target_` 即可,~150 行)。

## 4. 不在范围内

- **跨 DBApp 事务** — checkout 一个 entity 涉及多个 shard 的场景。
  Atlas 当前 entity 模型每个 entity 一个 dbid,不跨行;不需要分布式
  事务。
- **DBApp 副本(replication)** — Phase 15 不做主从复制,DBApp 内部
  仍是 SQLite/MySQL 自己的 storage。replication 由 DB 层(MySQL
  cluster / Postgres streaming)解决,Atlas 上层不关心。
- **shard auto-rebalance** — 自动按负载迁移 shard。先实现"手动 ops
  触发 rebalance" + "DBApp 死亡时 reassign",auto-rebalance 推到
  Phase 16+。
- **历史数据迁移工具** — 从单 DBApp 升到多 DBApp 的迁移脚本。当前
  Atlas 还在开发期,deferr 到生产部署前。

## 5. 消息协议草案

新增 message_id 段。Login 已占 5000-5999,DBAppMgr 使用 8000-8099:

```cpp
enum class DBAppMgr : uint16_t {
  kRegisterDbApp = 8000,
  kRegisterDbAppAck = 8001,
  kInformLoad = 8002,
  kGetShardTable = 8010,
  kShardTableResponse = 8011,
  kShardTableUpdate = 8012,
  kHealthProbe = 8020,
  kHealthProbeAck = 8021,
};
```

ShardTableResponse 结构(简化):

```
ShardTableResponse {
  uint32_t version;
  vector<ShardEntry> entries;
}
ShardEntry {
  uint64_t low_dbid;
  uint64_t high_dbid;    // exclusive
  uint16_t dbapp_id;
  Address dbapp_addr;
  bool is_retiring;
}
```

## 6. 与 Phase 13 follow-up 的关系

Phase 13 follow-up 列表里的 #4 就是本文档。完成 P15.1 + P15.2 后,
Phase 13 的 "Manager HA 三件套" 才齐(CellAppMgr / BaseAppMgr / DBAppMgr
都靠 worker-重建恢复 + Reviver supervision,无 snapshot)。

## 7. 工作量估计

| 阶段 | 内容 | 估计 |
|---|---|---|
| P15.1-D1 | DBAppMgr 进程骨架 + RegisterDbApp + InformLoad | 已起步，含 DBApp 注册接入 |
| P15.1-D2 | Shard table + 客户端 GetShardTable + broadcast invalidate | 已起步 |
| P15.1-D3 | BaseApp / LoginApp 接入 shard table 缓存,dual-path 兼容 (旧 single-dbapp 仍工作) | ~400 行 |
| P15.1-D4 | DBApp 端 idempotency cache + request retry on death | ~500 行 |
| P15.2-S1..S4 | HA(镜像 BaseAppMgr B1-B4) | ~3000 行 |
| 单测 + 集成测试 | | ~1500 行 |
| 文档 | phase15 主文档 + verify 脚本 + ops 指南 | ~800 行 |
| **总计** | | **~7600 行** |

## 8. 不会做的事(明确归档)

- 不会在 Phase 15 里替换 SQLite/MySQL 后端为分布式 DB(如 TiDB)。
- 不会改 Phase 7 已经稳定的 DBApp <-> DB 接口。
- 不会强制 LoginApp 必须经过 DBAppMgr — LoginApp 直连唯一 DBApp 的
  fast path 在单 DBApp 部署下保留;multi-DBApp 部署强制走 DBAppMgr。
- 不会让 DBAppMgr 持有任何 entity 数据 — DBAppMgr 只是路由表 + 健康
  状态,不接触 entity blob。

## 9. 决策检查清单(实现前需要确认)

- [x] dbid 编码方案:`dbapp_id` 进高位还是用独立路由表? → 当前倾向
      独立路由表(留出 dbid 全 64 位空间)。
- [x] machined 是否要新增 ProcessType::kDBAppMgr,还是复用 kDBApp? →
      新增 kDBAppMgr 与其他 mgr 一致。
- [ ] Reviver 是否同时启动 DBAppMgr cold start? → 是,加
      `--revive-dbappmgr-on-start` 与 cellappmgr / baseappmgr 对齐。
- [x] Shard table 是否在 watcher 中暴露(便于 ops debug)? → 是,
      `dbappmgr/shards/table` 摘要 + `shards/version`。
- [ ] verify_dbappmgr_ha.py 是否复用 verify_baseappmgr_ha 的 90% 代码,
      还是抽 verify-common 模块? → 先复用 + copy,等第三个 mgr verify
      后再做共享(参考 second-use 原则)。
