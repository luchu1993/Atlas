# Phase 11: 分布式空间 — Real/Ghost + CellAppMgr

**Status:** ✅ 主线已落地；✅ BigWorld-style CellApp LB / retire drain /
CellAppMgr HA 已进入可验证状态。当前 CellAppMgr 已覆盖注册、`app_id`
分配、Space bootstrap、BSP 负载均衡、late CellApp elastic split、自动
split / merge、retire handoff / drain、geometry 广播、`ShouldOffload`
freeze/unfreeze、CellApp 死亡 rehome、CellAppMgr Snapshot / Restore、
Reviver 重启、direct heartbeat、manager health audit、liveness / registry audit
和 Reviver leader lock。存活 CellApp 会订阅 CellAppMgr birth / death，在
mgr 被 Reviver 重启后主动重连并重新注册，HA reattach 可在 live cluster
收敛。
剩余工作集中在更大规模连续 rebalance / retire / crash 基线、
完整 `EntityBoundLevels` 等价能力和
跨机器共享 snapshot / 外部 leader lock。
**前置依赖:** Phase 10 (CellApp 单机)、Phase 9 (BaseAppMgr)
**BigWorld 参考:** `server/cellapp/real_entity.hpp`,
`entity_ghost_maintainer.cpp`, `server/cellappmgr/`

## 目标

将单 CellApp 扩展为多 CellApp 分布式空间。一个 Space 可被 BSP 树分区到
多个 CellApp 上；实体跨 Cell 边界时通过 Real/Ghost 机制保持 AoI 可见性；
Offload 实现无缝迁移。当前实现已可运行、可压测，并通过 MVP Unity bot
和 live retire-drain smoke 验证；后续重点是扩大验证规模并补齐少数
BigWorld 高阶能力。

## 当前已落地能力

| 能力 | 当前状态 |
|---|---|
| CellApp 注册 | CellAppMgr 分配 `app_id ∈ [1,255]`，回包携带 `game_time` 和 tick alignment epoch；CellApp 订阅 CellAppMgr birth / death，manager 重启后会断开旧 channel、清空旧 `app_id` 并重新注册 |
| Space 创建 | `CreateSpaceRequest.initial_cell_count` 支持多 Cell bootstrap，按新鲜 load、负载和 `app_id` 选 host |
| 启动缓冲 | CreateSpace 可等待 startup quiescence window，让 stagger-launched CellApp 一起参与初始分区 |
| 负载上报 | CellApp 上报 EWMA `load`、real entity count、per-cell script / native tick、witness / AoI / backup bytes、entity count、30-sample median，以及 X/Z 8-bucket entity / tick-cost shape；CellAppMgr 只接受匹配已注册 channel / address 的 `InformCellLoad` |
| BSP 均衡 | CellAppMgr 每 30 tick 运行 `BSPTree::Balance(0.9)`，按加权 leaf load 和 X/Z bucket shape 移动已有 split line |
| late join / 自动扩缩容 | 新 CellApp 注册后，已存在 Space 可把最重 leaf split 到新 host；持续热点 leaf 可自动 split 到 load 新鲜的空闲 CellApp；持续空闲 sibling leaf 可自动 merge；退役 CellApp 不再承接新 cell，空 leaf 可移除，非空 leaf 可 handoff 后 drain；primary leaf handoff 先拉 SpaceData snapshot 再发布 geometry |
| geometry 广播 | `BroadcastGeometry` 缓存 BSP blob；拓扑未变时不重发 CellApp geometry，BaseApp debug payload 独立按负载变化更新；deferred `AddCellToSpaceAck` 必须来自已 reattach 的目标 CellApp channel / address 且 `success=true` 才会发布拓扑 |
| offload freeze | geometry 广播按 `ShouldOffload(false, epoch) → UpdateGeometry(version) → ShouldOffload(true, epoch)` 顺序发送；CellApp 忽略旧 epoch |
| LB watcher | `cellappmgr/lb/cellapps`、`cellappmgr/lb/spaces`、`cellappmgr/lb/last_decision`、`decision_history`、`decision_count`、`pending_geometry_broadcasts`、`pending_space_create_status`、`load_report_stale_count`、`retire/status` 暴露 host/leaf load、load report age / stale 状态、tick load、`script_us`、`native_us`、witness / AoI / backup 指标、median、entity / load bucket shape、hot/idle ticks、geometry version、split / merge / retire / rehome 决策、retire/drain 状态、stuck drain、pending Space create 和 pending ack 数 |
| offload 校验 | `OffloadEntity` 携带 target cell 和 geometry version；接收端拒绝 stale geometry / target missing，Ack 带 reject reason |
| CellApp peer 信任边界 | Ghost、Offload、SpaceData、ClientRpcBroadcast 等 CellApp↔CellApp 入站消息按 machined CellApp registry 校验来源；未注册来源会被丢弃并计入 `cellapp/untrusted_cellapp_messages_total` |
| Real/Ghost | `GhostMaintainer` 维护 haunt，`OffloadChecker` 基于 BSP primary cell 触发迁移 |
| C# Ghost mirror | 每个 C++ Ghost 可有 C# passive mirror；`IsGhost` 跳过 tick / destroy / publish，只保留 ghost hook 和转发能力 |
| CellApp 死亡 | CellAppMgr 对死亡 leaf 做 `Unsplit` 或 rehome，并向 BaseApp 发送带 leaf bounds 的 `CellAppDeath`；BaseApp 按最后 cell pose 选精确 leaf host，restore 到已有 Ghost 时先 Ghost→Real，再优先用 Base cached backup、缺失时要求 Ghost backup |
| 客户端 debug | `SpaceBspGeometry` leaf 携带 owner、bounds、load 和 entity count；Unity gizmo 按负载染色 |

## 关键设计决策

### CellAppMgr 是 BSP 权威

CellAppMgr 拥有每个 Space 的 `BSPTree`。CellApp 只消费 `UpdateGeometry`
里的序列化 BSP，并用它驱动 `GhostMaintainer` / `OffloadChecker`。负载、
边界移动、leaf host 归属都以 CellAppMgr 为准。

### C# Ghost 是被动镜像

早期设计把 Ghost 定义为纯 C++ 容器；当前代码已扩展为 BigWorld-style 的
C# passive mirror。Ghost C# 实例通过 `RestoreGhost` 创建，`IsGhost=true`
后不跑普通 `OnTick`、不执行 Real 的 `OnDestroy`、不发布复制帧。它只承载
`OnGhostInit` / `OnGhostDestroy` 和必要的 Ghost→Real 方法转发。

这保留了 BigWorld 的"Ghost 不是权威逻辑 owner"约束，同时给脚本层留下
只读镜像和转发钩子。

### Offload 复用完整实体序列化

不迁移 GCHandle。Offload 发送方用 `SerializeEntity` 拿完整状态 blob，
接收方用 `RestoreEntity` 重建 Real。Real→Ghost 前调用
`EntityMigratingOut` 让 C# 侧停止 Real tick；随后 C++ `ConvertRealToGhost`
清 witness / controllers，必要时再创建 C# Ghost mirror。

接收方若已有 Ghost，则先销毁 C# Ghost mirror，再 `ConvertGhostToReal`
并 `RestoreEntity`。失败或超时通过 `RevertPendingOffload` 回滚本地 Ghost。

### BSP 负载均衡当前是简化版

Atlas 当前没有 BigWorld 的多级 `EntityBoundLevels`。BSP split line 由
sibling weighted load 差驱动；有 X/Z bucket shape 时，连续 Balance 会按
tick-cost bucket 估算新边界，late join split 也优先使用 tick-cost bucket；
没有 tick-cost 样本时回退 entity bucket、median 或原比例移动。方向反转会进入短 cooldown，抑制
采样噪声导致的 split line 来回震荡。

### CellApp 死亡恢复分层

CellAppMgr 只负责拓扑 rehome：死亡 leaf 被 sibling subtree 吸收，或被重新
指向幸存 CellApp。Real 恢复由 BaseApp 根据 `BackupCellEntity` 缓存的
`cell_backup_data` 发起。当前仍存在备份窗口和 rehome target 不可达导致的
Real lost 场景，完整故障恢复会在本 Phase 的后续里程碑和 Phase 13 一起收敛。

## 协议

### Inter-CellApp（Real ↔ Ghost）

| 消息 | ID | 方向 | 用途 |
|---|---|---|---|
| `CreateGhost` | 3100 | Real → Ghost CellApp | 创建 Ghost；携带 `other_snapshot` 初态、序号、`real_cellapp_addr` 和 Ghost backup blob |
| `DeleteGhost` | 3101 | Real → Ghost | 删除 Ghost |
| `GhostPositionUpdate` | 3102 | Real → Ghost | volatile 位置 / 方向，latest-wins |
| `GhostDelta` | 3103 | Real → Ghost | 按 other 受众过滤的 `other_delta` + `event_seq` |
| `GhostSetReal` | 3104 | 新 Real → Ghost | Offload 后通知新 Real 地址 |
| `GhostSetNextReal` | 3105 | 旧 Real → Ghost | Offload 前通知即将迁移 |
| `GhostSnapshotRefresh` | 3106 | Real → Ghost | Ghost history 断档时重灌 other snapshot，也可周期刷新 Ghost backup blob |

### Offload

| 消息 | ID | 方向 | 用途 |
|---|---|---|---|
| `OffloadEntity` | 3110 | 旧 → 新 CellApp | 完整 Real 数据、owner/other snapshot、Controller 状态、haunt 列表、witness 配置 |
| `OffloadEntityAck` | 3111 | 新 → 旧 | 成功 / 失败确认 |
| `baseapp::CurrentCell` | 2012 | 新 CellApp → BaseApp | 通知 Base 新 Cell 地址和 `cell_epoch` |
| `baseapp::CellEntityCreateFailed` | 2033 | CellApp → BaseApp | `CreateCellEntity` 拒绝回包；用于 death restore lost 计数 |

### CellAppMgr

| 消息 | ID | 方向 | 用途 |
|---|---|---|---|
| `RegisterCellApp` / `RegisterCellAppAck` | 7000 / 7001 | 双向 | 注册、`app_id` 分配、tick alignment |
| `InformCellLoad` | 7002 | CellApp → Mgr | EWMA load、entity count、per-cell weighted metrics、`script_tick_us` / `native_tick_us`、median、X/Z entity buckets 和 tick-cost buckets |
| `CreateSpaceRequest` | 7003 | BaseApp / 脚本 → Mgr | 创建 Space |
| `AddCellToSpace` / `AddCellToSpaceAck` | 7004 / 7008 | 双向 | 分配 Cell，并确认 receiver 已建好本地 Cell |
| `RemoveCellFromSpace` | 7009 | Mgr → CellApp | 删除已 drain 为空的本地 Cell |
| `RequestCellAppState` | 7010 | Mgr → CellApp | 要求 CellApp 立即用 `InformCellLoad` 回报 load 和本地 geometry version |
| `HealthProbe` / `HealthProbeAck` | 7011 / 7012 | Reviver ↔ Mgr | Reviver 直接二进制 heartbeat；Ack 携带 game time 和 snapshot save / failure / dirty / stale 摘要 |
| `UpdateGeometry` | 7005 | Mgr → CellApp | BSP 树 / Cell 边界更新 |
| `ShouldOffload` | 7006 | Mgr → CellApp | geometry 广播前后 freeze / unfreeze 本 Cell 的 offload |
| `SpaceCreatedResult` | 7007 | Mgr → 请求方 | 创建结果回包 |
| `baseapp::CellAppDeath` | 2026 | Mgr → BaseApp | CellApp 死亡 rehome 通知 |

## BigWorld 对齐状态

### M1 — LB 可观测性

补齐解释能力，让每次 split / balance / rehome 都能从 watcher 或日志还原。

交付：
- 已落地：CellAppMgr watcher 暴露 per-space leaves、cell load、entity count、
  median、geometry version、pending AddCell ack / Space create 数和 CellApp
  load report 新鲜度。
- 已落地：`SpaceBspGeometry` debug 路径携带 load/entity_count；拓扑未变但负载
  变化时只更新 BaseApp / Client debug payload，不重发 CellApp `UpdateGeometry`。
- 已落地：`cellappmgr/lb/last_decision`、`decision_history`、`decision_count`
  暴露最近 16 次 balance、split、merge、retire handoff / drain、CellApp death
  rehome 的 key-value 决策摘要，并带 `before_leaves` / `after_leaves`、
  `before_version` / `after_version` 等紧凑拓扑差异；deferred 或批量广播路径会在
  detail 中标出 pending 状态。`leaf_changes` 记录变化 leaf 总数，`leaf_diff`
  输出前 8 个变化 leaf 的 app / load / entity / bounds 前后值。
- 当前边界：决策历史是内存短历史；`leaf_diff` 是 bounded 诊断摘要，不是持久审计日志。

验收：压测中不看 CellApp stdout，也能判断 LB 为什么移动边界或为什么没有移动。

### M2 — 迁移一致性

在复杂 LB 前先把拓扑变化和 offload 的乱序风险关住。

交付：
- 已落地：`UpdateGeometry` 带 monotonic geometry version；CellApp 忽略旧版本。
- 已落地：`InformCellLoad` per-cell report 带 geometry version；CellAppMgr 忽略版本不匹配的 per-cell load。
- 已落地：`OffloadEntity` 带 target cell id 和 geometry version；接收端按本地 geometry 和 local cell 严格校验。
- 已落地：`ShouldOffload` 扩展为 freeze epoch，receiver 忽略旧 enable。
- 已落地：`OffloadEntityAck` 区分 `rejected`、`stale_geometry`、`target_missing`、`restore_failed`。

验收：连续 geometry 广播、旧 offload 延迟到达、target cell 未 ready 都不会产生
双 Real 或半迁移状态。

### M3 — 负载模型升级

从单一 tick EWMA + entity count 扩展成 BigWorld 风格的空间负载画像。

交付：
- 已落地：CellApp per-cell 上报 managed entity tick cost、C++ native pump cost、
  witness count、AoI peer count、reliable/unreliable AoI bytes 和 backup bytes。
- 已落地：CellAppMgr 使用加权 load 评分，保留 tick EWMA 作为 leaf load 下限。
- 已落地：负载权重通过 config / watcher `cellappmgr/lb/weights/*` 可调。
- 已落地：`AtlasReportScriptTick` 从 C# `OnTick` + component tick 采样
  per-entity 微秒数，`InformCellLoad` 带 `script_tick_us`，无采样时回退到
  CellApp EWMA 按 real entity share 分摊。
- 已落地：CellApp 对 controller tick、Witness pump、backup pump 和 owner baseline
  pump 的 C++耗时按 local cell 归因，`InformCellLoad` 带 `native_tick_us`；
  `tick_load` 使用 managed + native 总耗时，缺样本时仍回退 CellApp EWMA。
- 已落地：`InformCellLoad` 发送失败时保留 cell / entity tick counters，
  下一次成功上报覆盖完整采样窗口，避免 LB 因短暂 mgr channel 失败丢热点样本。
- 已落地：CellApp watcher `cellapp/inform_cell_load_send_failures_total`
  暴露 load report 发送失败次数，便于定位 CellAppMgr LB 样本缺失。
- 已落地：CellAppMgr watcher `cellappmgr/lb/load_report_stale_count`
  和 `cellappmgr/lb/cellapps` 的 `load_age_ms` / `load_stale` 暴露陈旧 LB 样本；
  判定阈值可通过 `cellappmgr/lb/load_report_stale_ms` 调整。
- 已落地：Space bootstrap、auto split target、retire drain target 和 death rehome
  host 选择会跳过 load report stale 的 CellApp；auto split / merge / empty-retire
  不使用 stale owner 样本推进拓扑，pending Space create 会等到有新鲜 host 后再执行。
- 已落地：`cellappmgr/lb/pending_space_creates` 和
  `pending_space_create_status` 暴露等待新鲜 host 或 startup quiescence 的
  CreateSpace backlog。

验收：实体集中导致 witness / AoI bytes 高的 cell 能被判为热点；脚本 CPU 低实体数
热点可通过 managed tick timing 进入 leaf load；纯 C++ controller / replication pump
热点也能进入 leaf `tick_load`。

### M4 — EntityBoundLevels 等价能力

补齐当前文档中明确省掉的 BigWorld LB 核心能力。

交付：
- 已落地：CellApp 为每个 local cell 输出 X/Z 方向 8-bucket entity histogram，
  并按 per-entity managed / native tick cost 生成 X/Z tick-cost buckets。
- 已落地：没有 tick-cost 样本时，CellAppMgr elastic split 按 entity histogram
  桶边界估算两侧实体量；桶为空时回退到 median / midpoint。
- 已落地：CellAppMgr elastic split 和连续 Balance 优先使用 tick-cost buckets
  估算边界；没有 tick-cost 样本时回退 entity buckets，再回退 median / midpoint
  或原比例移动。
- 已落地：repeated balance 在方向反转时进入 cooldown，避免 split line 来回震荡。

验收：双峰分布、边界热点和稀疏空洞场景下，split line 能落在实际负载分界附近。

### M5 — 自动拓扑生命周期

把当前 bootstrap + late join split 扩展为自动 split / merge / retire。

交付：
- 已落地：高负载 leaf 持续超阈值时自动 split 到空闲 CellApp，并复用
  AddCell ack 后再广播 geometry 的安全握手。
- 已落地：自动 split 阈值通过 `cellappmgr/lb/auto_split/*` watcher 可调；
  `cellappmgr/lb/spaces` 暴露 leaf `hot_ticks`。
- 已落地：非 primary sibling leaf 长期低负载且 `entity_count == 0` 时自动
  `Unsplit` merge；mgr 广播新 geometry 后用 `RemoveCellFromSpace` 删除空 cell。
- 已落地：自动 merge 阈值通过 `cellappmgr/lb/auto_merge/*` watcher 可调；
  `cellappmgr/lb/spaces` 暴露 leaf `idle_ticks`。
- 已落地：`cellappmgr/lb/retire/app_id` 可标记 CellApp 退役；退役节点不再参与
  新 Space bootstrap、late join grow 或自动 split target，设置为 `0` 可清除退役标记。
- 已落地：退役 CellApp 上的空非 primary leaf 可在 LB tick 中 `Unsplit` 移除；
  mgr 广播新 geometry 后用 `RemoveCellFromSpace` 删除本地空 cell。
- 已落地：退役 CellApp 上的非空 leaf 可先 handoff 到非退役 CellApp；
  新 host ack `AddCellToSpace` 后，mgr 把新 geometry 同步给新旧 host，旧 host 通过
  OffloadChecker 迁出 Real，旧 host 上报该 cell 为空后再 `RemoveCellFromSpace`。
- 已落地：primary leaf handoff 的 `AddCellToSpace` 标记 primary，但不重复携带
  `space_master_type`，避免 handoff 时重复创建 SpaceMaster。
- 已落地：单 leaf Space 的 primary drain 支持 snapshot-gated handoff；`AddCellToSpace`
  携带旧 owner 地址，目标 CellApp 拉取 `SpaceDataSnapshot` 后才 ack mgr，mgr 再发布
  primary handoff geometry。
- 已落地：`cellappmgr/lb/retire/status` 暴露每个 retiring app 的 owned leaves、
  active drains、pending geometry 和 `ready`，作为运维下线判定。
- 已落地：retire drain watchdog 通过 `cellappmgr/lb/retire/drain_watchdog_ms`
  配置阈值；超过阈值后 `stuck_count` 和 `retire/status` 标出 stuck drain，
  并周期性写 warning 日志。
- 已落地：CellAppMgr 单测覆盖同一 retiring host 从多个 Space 连续 handoff、
  ack geometry、drain 清空并进入 ready 状态。
- 已落地：`tools/bin/verify_retire_drain.{bat,sh}` 可在 live world_stress 集群上
  通过 watcher 标记持有 BSP leaf 的 retiring CellApp，并验证多 Space drain
  最终 `ready=1`；`--cycles N` 可连续退役当前持 leaf 最多的 CellApp，并验证
  每轮 LB decision watcher 推进。
- 已实测：8 clients / 2 Spaces / 3 CellApps 的 live 集群中，`echo_sent=81`、
  `echo_received=81`、`echo_loss=0`；retiring app 从 2 个 leaf drain 到
  `owned=0 drains=0 pending=0 ready=1 stuck=0`。
- 已实测：同规模 live 集群 `verify_retire_drain --cycles 2` 通过，world_stress
  `echo_sent=113`、`echo_received=113`、`echo_loss=0`；两轮 retire 均收敛到
  `ready=1 stuck=0`，`decision_count` 分别从 `2→6`、`8→10`。
- 当前边界：小规模 live-cluster 已验证 retire drain；连续 retire verifier 已就绪，
  仍缺更大规模性能和稳定性基线。

验收：新 CellApp 加入后能持续吸收热点；空闲 CellApp 可被迁空；merge 不产生
offload pingpong。

### M6 — CellApp 故障恢复升级

把当前 backup-based restore 收敛到更接近 BigWorld 的恢复语义。

交付：
- 已落地：BaseApp death restore 发到幸存 CellApp 时，若目标已持有同 entity 的
  Ghost，`CreateCellEntity` 会先销毁 C# Ghost mirror，再 Ghost→Real，并用
  cached `cell_backup_data` 恢复脚本实例。
- 已落地：`AddCellToSpace` 到达时会把已存在但尚未挂入 local Cell 的 Real
  补进对应 Cell，覆盖 death restore 与 AddCell 乱序。
- 已落地：`BackupCellEntity` 携带最后 cell pose；`CellAppDeath` 携带 rehome 后的
  leaf bounds / host，BaseApp restore 会按 entity position 选择精确 leaf host。
- 已落地：BaseApp watcher 暴露 CellApp death notifications、restore scheduled
  及其 payload / Ghost backup 来源拆分、restored、lost、timeout、pending、
  last / max restore elapsed 和聚合 status；
  `baseapp/cellapp_routes` 按 CellApp address 汇总当前 BaseEntity route，供
  live verifier 区分可恢复实体和 CellApp-only 实体，并暴露
  payload / Ghost backup restore candidate 计数；
  CellApp watcher 暴露 CreateCellEntity 通用来源计数，以及 death-restore 专用
  payload / Ghost backup / empty / failure / Ghost→Real promote 计数。
- 已落地：`CreateGhost` 携带 Cell 持久化 blob；Real 会按 backup cadence 通过
  `GhostSnapshotRefresh` 刷新 Ghost backup。
- 已落地：BaseApp 缺少 cached `cell_backup_data` 时可发起 Ghost-only restore；
  CellApp 只在目标已有 Ghost 且有 Ghost backup 时才提升，避免凭空创建空状态 Real。
- 没有 Ghost 时仍走 BaseApp cached `cell_backup_data` restore；没有 backup 也没有
  Ghost backup 时 CellApp 会用 `CellEntityCreateFailed` 显式拒绝并保持无 Real。
- 恢复后通过 `CellEntityCreated` ack 重装 BaseApp route；有客户端绑定时复用正常
  `EnableWitness` / baseline / SpaceData init 路径。
- 已落地：death restore pending request 有超时扫尾；目标 CellApp 没回
  `CellEntityCreated` / `CellEntityCreateFailed` 时会计入 lost 和 timeout。
- 已落地：`tools/bin/verify_cellapp_rehome.{bat,sh}` 可在 live world_stress
  集群上 abnormal shutdown 一个持 leaf 的 CellApp，并验证 CellAppMgr BSP
  rehome、LB decision watcher，以及 BaseApp death notification、scheduled /
  restored / lost / timeout / pending restore watcher 收敛；`--cycles N` 可连续
  注入多轮 CellApp crash，`--max-restore-ms` 可约束本轮 restore resolution 耗时；
  `--min-restores N` 可要求每轮至少调度 N 个 BaseApp death restore，避免空拓扑
  rehome 被误当成实体恢复基线，并让自动目标选择优先命中持有足够
  BaseApp route 的 CellApp；多轮完成后会汇总 scheduled / restored、
  payload / Ghost backup 来源、empty / failure、promote 和有实体恢复轮次的
  restore elapsed sample count / avg / p50 / p95 / max；
  `--allow-no-baseapp` 只保留拓扑 smoke，不可与实体恢复闸门组合；
  verifier 还会校验幸存 CellApp 的 death-restore payload / Ghost backup 来源计数
  分别覆盖 BaseApp 本轮 payload / Ghost backup scheduled restore，且
  death-restore empty / failure 不增长；
  `--min-ghost-backup-restores N` 会按 `baseapp/cellapp_routes` 的
  `ghost_backup_candidates` 选择目标，并把 Ghost backup fallback 覆盖纳入闸门；
  `--min-payload-restores N` 会按 `payload_candidates` 选择目标，并把
  Base cached payload 恢复覆盖纳入同一生产基线；`--min-promoted-restores N`
  会要求实际 death restore 至少发生 N 次 Ghost→Real promotion；
  `--summary-json PATH` 可在全部 cycles 通过后写出机器可读恢复指标。
- 已实测：3 CellApps / 2 Spaces live 集群中，`cellapp_02 app_id=3` 从 2 个
  leaf rehome 到 0 个，`decision_count` 从 `4→6`，最后一次决策为
  `action=rehome reason=cellapp-death`。

验收：CellApp crash 后统计 `promoted/restored/lost`；有 Ghost backup 的实体在
Base cached backup 缺失时也能恢复。

### M7 — CellAppMgr HA

与 Phase 13 对齐，补齐 manager 崩溃后的状态恢复。

交付：
- 已落地：`CellAppMgr::Snapshot()` / `Restore()` 覆盖 cellapps、spaces BSP、
  next ids、leaf load profile（含 native tick 和 tick-cost buckets）以及
  pending geometry broadcasts 和 retire drain 状态。
- 已落地：CellAppMgr 支持 `--snapshot-path` 周期性写本地 HA snapshot；
  CellApp 注册、geometry 发布、deferred geometry ack / timeout、retire 流程、
  CellApp death 和 registry reconcile 会标记 dirty snapshot，并按最多 1s
  节流提前落盘；受控 shutdown 前会再 flush 一次；新进程启动时会先尝试读取并
  `Restore()`。
- 已落地：Reviver 启动新 CellAppMgr 时传递 snapshot path 和 interval；
  `cellappmgr/ha/snapshot_interval_ms` 可验证新进程实际配置。
- 已落地：Reviver 使用 `--config` 启动时会把同一配置文件传给 revived
  CellAppMgr，再用 CLI 覆盖目标进程身份和端口，避免 LB 权重等运行配置漂移。
- 已落地：snapshot 文件使用 magic / version / payload size / checksum
  envelope；当前开发期 schema 持久化 `native_tick_us`、tick-cost buckets 和
  retire drain 状态。写入通过 tmp 文件和平台原子替换完成，所有成功落盘都会
  递增 `snapshot_saves`，并保留上一份 `.bak`；启动恢复主文件失败时会回退到
  通过完整校验的 `.bak`，并通过 `snapshot_fallback_restores` 单独记录，不计入硬失败。
- 已落地：启动恢复时 snapshot 缺失可跳过；主备 snapshot 均损坏、
  checksum 不匹配或版本不兼容时会失败启动，避免 CellAppMgr 在未知旧状态上
  继续提供权威 BSP。
- 已落地：Restore 校验 `app_id` 唯一性、finite load、leaf host、pending
  geometry 和 retire drain 的跨表引用；语义不一致的 snapshot 会被拒绝。
- 已落地：restore 后的 CellApp 重新注册会保留原 `app_id`、重挂 channel，
  并重发本地 leaf 的 `AddCellToSpace` 与当前 `UpdateGeometry`。
- 已落地：存活 CellApp 订阅 CellAppMgr birth / death；Reviver 拉起新 mgr 后，
  CellApp 会断开旧 mgr channel、清空旧 `app_id` 并重新注册，让
  `reattach_pending` 收敛到 0。同一 pid / 地址的 birth replay 会被忽略，
  避免重复断连重注册；同端口新 pid 仍会触发重连。
- 已落地：restore 后尚未 reattach 的 CellApp 不参与新 Space bootstrap、
  auto split 或 CellApp death rehome；其 `InformCellLoad` 会被忽略，避免把
  拓扑或负载分配给不可达 host。
- 已落地：restore 后还有 reattach pending 时，CellAppMgr 会冻结 LB tick、
  elastic grow、auto split / merge 和 retire drain 拓扑推进；pending
  `AddCellToSpaceAck` 未完成的 Space 不再提前移动 BSP 边界，且恢复出来的
  pending geometry 不会在目标 CellApp reattach 前通过 timeout fallback 发布。
- 已落地：`cellappmgr/ha/reattach_status`、`reattach_pending`、
  `reattach_completed_count`、`reattach_completed`、`reattach_stuck` 和
  `reattach_state`、`reattach_watchdog_ms`、`restore_gate_active`、
  `restore_gate_blocked_pending_geometry` 和 `restore_gate_status` 暴露 HA
  restore 后旧 CellApp 的回归收敛状态和恢复期 LB gate；
  超时未回归的 host 会被标为 stuck 并写 warning。
- 已落地：reattach pending 期间会审计 machined CellApp registry；已消失且
  不持有 leaf 的 restored host 会被清理，有可用 survivor 时会复用
  CellApp death rehome 路径，缺少可接管目标时保持 restore gate closed 并通过
  `reattach_registry_status` 暴露 blocked。
- 已落地：reattach replay 后 mgr 发送 `RequestCellAppState`，CellApp 绕过
  load 节流立即用 `InformCellLoad` 回传最新 load、local cell 和 geometry version。
- 已落地：新 mgr 通过 reattach replay 的 full `UpdateGeometry` 让 CellApp
  以 mgr 为权威重同步。
- 已落地：最小 Reviver 进程按 BigWorld 风格监控全局唯一 CellAppMgr；
  异常 death 后按受控连续次数重启同名 mgr，并把 snapshot path 传给新进程；
  新 mgr direct heartbeat ack 后重置连续重启预算；启动后未在
  `revive-cellappmgr-launch-timeout-ms` 内注册会按失败重试，预算耗尽时通过
  watcher 暴露 `status=restart_limited`、`restart_limit_reached` 和
  `restart_limit_hits`。同 pid / 地址的 birth 或 query replay 会被 Reviver
  幂等忽略，不会重置 heartbeat 状态或递增 active generation。
- 已落地：持锁 Reviver 会直接向目标 CellAppMgr 发送 `HealthProbe` heartbeat，
  并通过 `app/uptime_seconds` watcher 检查事件循环响应性；对本地目标检查
  PID liveness，并周期性审计 machined registry。活跃 CellAppMgr 连续缺失时，
  即使没有收到 listener death 通知也会按异常路径重启。本地目标 health
  failure、manager health watcher timeout 或 registry missing 达到阈值时，
  Reviver 会终止旧 pid 后再重启。
  direct heartbeat 有独立响应超时，并与 manager watcher 使用独立连续失败计数；
  `HealthProbeAck` 还带回目标 mgr 的 snapshot save、failure、dirty 和 stale
  摘要，Reviver 通过 `heartbeat_last_ack_age_ms` 和
  `heartbeat_snapshot_status` 汇总 watcher 可在 machined watcher 之外观察
  heartbeat 与 HA snapshot 是否仍新鲜。
  旧 pid 会先被终止，避免挂死进程继续占用 internal port。
- 已落地：MachinedClient async registry query 和 watcher query 有本地超时，
  避免 machined 连接保持但请求无回复时让 Reviver audit pending 卡死。
- 已落地：machined 在同名注册时会清理 PID 已死亡的 stale entry，避免旧
  registry 阻塞新 CellAppMgr 注册。
- 已落地：machined 转发 `shutdown` 后保留目标 reason，目标随后 deregister /
  disconnect / heartbeat timeout 时的 `DeathNotification` 会携带同一 reason；
  abnormal shutdown 可稳定触发 Reviver restart，reason 0 保持正常退出语义。
- 已落地：Reviver 默认获取目标 CellAppMgr 的进程级 leader lock；同机多
  Reviver 只有持锁实例会 cold-start / restart mgr，其余实例保持 standby
  并通过 watcher 暴露 leader 状态。
- 已落地：`run_world_stress.py --with-cellappmgr-reviver` 可直接拉起 Reviver、
  CellAppMgr snapshot 和 leader lock；`--cellappmgr-reviver-count N` 可启动共享
  snapshot / leader lock 的多 Reviver leader / standby 拓扑；结束时会通过
  machined 关闭 revived CellAppMgr，避免 Reviver 启动的 mgr 成为残留进程；
  Reviver 拉起的新 CellAppMgr stdout / stderr 会写入独立日志，避免混入 Reviver
  自身日志。
- 当前边界：Reviver 只覆盖全局唯一 CellAppMgr；跨机器外部 leader lock、
  共享 snapshot 以及 BaseAppMgr / DBAppMgr 接管归 Phase 13。

验收：kill CellAppMgr 后，Reviver 拉起新 mgr，已有 Space 不丢，CellApp 不需要
整体重启。

## 当前边界

- BigWorld `EntityBoundLevels` 完整等价能力未实现；当前已有 X/Z 8-bucket
  entity histogram 和 per-bucket tick-cost shape，并用于 elastic split 和连续
  Balance 的粗粒度边界估算。
- 自动 split、空 leaf merge、retire no-new-cell、空 leaf 移除、非空 leaf
  handoff drain、primary handoff、snapshot-gated single-leaf drain 和 retire
  ready 判定已落地；多 Space 连续 retire 已有单测和小规模 live-cluster
  实测，仍缺更大规模基线。
- Offload/geometry 已有 version / freeze epoch；还需要更大规模连续 rebalance 压测覆盖。
- CellApp 死亡恢复已有 Ghost backup fallback；成功由 `CellEntityCreated` 计数，
  失败由 `CellEntityCreateFailed` 或 pending restore timeout 计数；live
  crash / rehome verifier 已覆盖拓扑 rehome、BaseApp notification、restore
  scheduled / restored / lost / timeout / pending 收敛和 restore elapsed 上限检查，
  仍缺更大规模实体恢复成功率基线。
- CellAppMgr HA 已有本地 Snapshot / Restore、snapshot 文件、最小 Reviver
  接管、Reviver leader lock、CellApp reattach geometry replay 和 CellApp
  侧 load / geometry version 重拉；live fault-injection 已验证 mgr 重启后
  存活 CellApp 自动重连并完成 reattach。snapshot 文件已有 envelope 校验、
  dirty topology flush 和原子替换，并会保存 native tick 与 tick-cost bucket
  负载形状，但仍不是 WAL，也不是跨机器共享存储。BaseAppMgr / DBAppMgr 接管归
  Phase 13。
- `EntityRangeListNode` owner 已改为强类型 `CellEntity*`；RangeList 节点
  仍只保存非拥有指针，空间库通过前置声明避免反向依赖 CellApp 实现。

## 验证基线

现有覆盖：
- `tests/unit/test_bsp_tree.cpp` 覆盖 split / balance / serialize / unsplit。
- `tests/unit/test_cellappmgr.cpp` 覆盖注册、负载、建 Space、死亡 rehome、geometry freeze。
- `tests/unit/test_cellappmgr.cpp` 覆盖 CellAppMgr Snapshot / Restore 对
  cellapps、Space BSP、next ids、leaf load profile 和 pending geometry broadcast
  的保留。
- `tests/unit/test_cellappmgr.cpp` 覆盖 CellAppMgr HA snapshot 文件 round-trip
  和成功保存计数 / primary file envelope readiness /
  `.bak` readiness summary watcher。
- `tests/unit/test_cellappmgr.cpp` 覆盖 CellAppMgr HA snapshot 主文件损坏或
  缺失时回退上一份 `.bak`，并区分 fallback restore、restore failure counter、
  最后恢复来源、fallback primary error、restore status summary、
  restore attempt / success age 与硬失败 watcher。
- `tests/unit/test_cellappmgr.cpp` 覆盖 CellAppMgr HA snapshot save 失败时的
  save failure counter 和 last save error watcher。
- `tests/unit/test_cellappmgr.cpp` 覆盖 CellAppMgr HA snapshot freshness / stale
  watcher。
- `tests/unit/test_cellappmgr.cpp` 覆盖 CellAppMgr HA 周期 snapshot 失败时按
  interval 节流重试。
- `tests/unit/test_cellappmgr.cpp` 覆盖 CellAppMgr HA dirty topology snapshot
  在周期 interval 前提前落盘，并清空 dirty watcher。
- `tests/unit/test_cellappmgr.cpp` 覆盖 CellAppMgr 受控 shutdown 前 flush
  最新 HA snapshot。
- `tests/unit/test_cellappmgr.cpp` 覆盖 CellAppMgr HA 当前 snapshot schema、
  unsupported version、checksum mismatch、损坏文件、重复 `app_id` 和 dangling
  leaf host 拒绝。
- `tests/unit/test_filesystem.cpp` 覆盖 HA snapshot 落盘依赖的原子文件替换。
- `tests/unit/test_cellappmgr.cpp` 覆盖 CellAppMgr HA restore 后 reattach
  pending / completed / stuck watcher 收敛。
- `tests/unit/test_cellappmgr.cpp` 覆盖 Snapshot / Restore 后 CellApp reattach
  保留原 `app_id` 并重放 `AddCellToSpace` / `UpdateGeometry` / state request。
- `tests/unit/test_cellappmgr.cpp` 覆盖 Snapshot / Restore 后未 reattach 的
  CellApp 不参与新 Space 分配。
- `tests/unit/test_cellapp_handlers.cpp` 覆盖 `RequestCellAppState` 触发
  CellApp 立即回传带 geometry version 的 `InformCellLoad`。
- `tests/unit/test_cellapp_handlers.cpp` 覆盖 CellAppMgr birth replay 判定：
  同 pid / 地址不重连，新 pid、新地址或旧 channel down 后会重连。
- `tests/unit/test_cellappmgr.cpp` 覆盖 per-cell weighted LB metrics、load report
  stale watcher、stale host 跳过和 watcher 输出。
- `tests/unit/test_cellappmgr_messages.cpp` / `tests/unit/test_cellappmgr.cpp` /
  `tests/unit/test_cellapp_handlers.cpp` 覆盖 `native_tick_us` wire round-trip、
  CellAppMgr watcher / leaf 存储，以及 managed tick 按 cell 上报。
- `tests/unit/test_cellapp_handlers.cpp` 覆盖 `InformCellLoad` 发送失败后保留
  managed tick counters、递增失败 watcher，并在重试成功时补报。
- `tests/unit/test_cellappmgr.cpp` 覆盖 LB `last_decision` / `decision_history`
  watcher 对 auto split、auto merge、retire empty remove、retire handoff 和
  drain complete 的结构化决策输出、紧凑前后差异字段和 per-leaf diff 摘要。
- `tests/unit/test_cellappmgr.cpp` 覆盖 bucket histogram 和 tick-cost bucket
  驱动的 elastic split 位置。
- `tests/unit/test_bsp_tree.cpp` / `tests/unit/test_cellappmgr.cpp` 覆盖 entity /
  tick-cost bucket 驱动的连续 Balance split line 移动。
- `tests/unit/test_bsp_tree.cpp` 覆盖 balance 方向反转 cooldown。
- `tests/unit/test_cellappmgr.cpp` 覆盖持续热点 leaf 自动 split 到空闲 CellApp。
- `tests/unit/test_cellappmgr.cpp` 覆盖持续空闲 sibling leaf 自动 merge。
- `tests/unit/test_cellappmgr.cpp` 覆盖 retire watcher、退役 host 跳过新 Space 分配、
  退役 host 上空非 primary leaf 的移除、非空非 primary leaf 的 handoff drain、
  primary leaf handoff 到同 Space replica、single-leaf primary handoff，以及 retire ready watcher。
- `tests/unit/test_cellapp_handlers.cpp` 覆盖 primary handoff `AddCellToSpace` 在
  `SpaceDataSnapshot` 落地后才 ack CellAppMgr。
- `tests/unit/test_cellapp_handlers.cpp` 覆盖未注册 CellApp 来源的 CreateGhost、
  SpaceDataUpdate 和 OffloadEntity 会被拒绝。
- `tests/unit/test_cellapp_handlers.cpp` 覆盖 `CreateCellEntity` 可把已有 Ghost
  提升为 Real，并在 `AddCellToSpace` 晚到时补齐 local Cell membership。
- `tests/unit/test_baseapp_messages.cpp` / `tests/unit/test_cellappmgr.cpp` 覆盖
  CellApp death rehome leaf bounds、按位置解析 leaf host 和 backup pose wire round-trip。
- `tests/unit/test_login_rollback.cpp` / `tests/unit/test_cellapp_handlers.cpp` 覆盖
  CellApp death scheduled/restored/lost/promoted watcher 计数和 BaseApp restore status。
- `tests/unit/test_cellapp_messages.cpp` / `tests/unit/test_intercell_messages.cpp` /
  `tests/unit/test_cellapp_handlers.cpp` 覆盖 Ghost backup wire format、
  Ghost-only restore 拒绝和 Ghost backup fallback restore。
- `tests/unit/test_baseapp_messages.cpp` / `tests/unit/test_login_rollback.cpp` 覆盖
  `CellEntityCreateFailed` wire format，以及 death restore pending 成功 / 失败 / 超时计数。
- `tests/unit/test_cellapp_handlers.cpp` 覆盖 `RemoveCellFromSpace` 删除空本地 Cell。
- `tests/unit/test_cellapp_native_provider.cpp` 覆盖 CellApp NativeApi 的
  `ReportScriptTick` 回调路由。
- `tests/unit/test_aoi_trigger_hysteresis.cpp`、`tests/unit/test_controller_codec.cpp`、
  `tests/unit/test_cellapp_native_provider.cpp` 和 `tests/unit/test_witness_*.cpp`
  覆盖 `EntityRangeListNode` typed owner 的 AoI、controller encode、
  proximity dispatch 和 Witness 生命周期路径。
- `tests/unit/test_cell.cpp` / `tests/unit/test_cellapp_handlers.cpp` 覆盖 freeze epoch、
  stale geometry 和 target-missing offload 拒绝。
- `tests/unit/test_baseapp_messages.cpp` 覆盖 `SpaceBspGeometry` load/entity_count wire round-trip。
- `tests/unit/test_intercell_messages.cpp` / `tests/unit/test_cellappmgr_messages.cpp` 覆盖
  geometry version、target cell 和 offload reject reason wire round-trip。
- `tests/csharp/Atlas.Client.Tests/ClientSessionTests.cs` 覆盖 C# client 解码 LB debug payload。
- `tests/integration/test_cellappmgr_integration.cpp` 覆盖真实 RUDP 注册、建 Space、
  elastic split、AddCell ack 和 timeout fallback。
- `tests/integration/test_cellappmgr_process.cpp` 覆盖 Reviver cold-start 和异常
  终止后重启真实 CellAppMgr 进程，以及同机多 Reviver leader lock 防止重复
  cold-start、late Reviver attach 不重启已有 mgr、Reviver 启动前目标已死时
  拉起新 mgr、leader 退出后 standby 接管不重启已有 mgr、目标 watcher 可响应
  但 direct heartbeat 不回包时 forced termination + restart、连续 no-ack manager
  达到 restart limit 后停止重启、manager health watcher pending timeout、
  launched process 不注册时重试到 restart limit，并验证 Reviver `status`、
  `launch_pending`、direct heartbeat last ack age、manager health、registry audit
  watcher 和 revived CellAppMgr 配置继承 / output path。
- `tests/integration/test_machined_registration.cpp` 覆盖 machined 转发 shutdown
  后，目标 deregister 产生的 death notification 保留原 reason。
- `tools/bin/verify_cellappmgr_ha.{bat,sh}` 用于 live cluster CellAppMgr HA
  fault-injection smoke：通过 machined 关闭 mgr，等待 Reviver 重启，并验证
  Reviver active generation 递增、direct heartbeat last ack age、manager health、
  snapshot save / restore watcher 健康、primary snapshot file envelope / checksum readiness
  （失败时暴露有界 `error_detail`）、
  primary snapshot file dry-run restore topology readiness（失败时暴露有界
  `error_detail`），且健康或允许缺失状态必须为 `error_detail=none`、
  snapshot freshness / stale 状态、
  `.bak` fallback envelope / checksum readiness（默认要求 `state=ready valid=1`）、
  `.bak` fallback dry-run restore topology readiness、
  Reviver leader lock 唯一 active leader、
  重启前后 `cellappmgr/lb/spaces` 的 Space / leaf topology fingerprint 一致性、
  kill 前最后一次成功落盘 snapshot 的 topology fingerprint 与当前 live topology
  一致性且 pending ack 为 0、
  restore 成功后 `snapshot_last_restore_topology` 与预期 topology fingerprint 一致性且
  pending ack 为 0、
  restore attempt / success age、restore status summary、
  fallback primary error、snapshot failure 聚合计数一致性、CellApp 自动重连、reattach completed、
  restored / pending / completed_count / state watcher 与 status summary 字段一致、
  stuck watcher 收敛，revived CellAppMgr 独立日志包含新 pid；
  snapshot save 校验允许历史累计失败，但会拒绝验证窗口内新增 save failure
  或 dirty snapshot 未清空，
  每轮 restore 收敛后还会等待 revived CellAppMgr 重新写出 fresh snapshot，并要求
  last save age 不大于 last restore age；
  `--no-inject` 也会校验聚合计数、freshness、reattach / restore gate /
  registry watcher 与 summary 一致性，以及当前拓扑可解析且无 pending ack。
  高 churn 压测可用 `--allow-topology-change` 显式跳过拓扑指纹一致性。
  收敛后等待稳定窗口确认没有二次 restart、heartbeat timeout 或 forced termination；
  `--cycles` 可连续注入多轮
  异常接管，`--no-inject` 可复用同一稳定窗口做非破坏性巡检；
  `--summary-json` 可在全部检查通过后写出机器可读接管摘要；
  `--max-takeover-ms` 可限制每轮 shutdown 到 revived mgr 写出 fresh snapshot 的
  takeover elapsed；`--min-revivers` 可要求验证拓扑包含 standby Reviver 并拒绝
  多 active leader。
- `tests/integration/test_offload_traversal.cpp` 覆盖实体跨 BSP split 后 offload。
- `tools/bin/run_mvp_cluster.bat` 用于 4 CellApp + NPC 的端到端 LB smoke。
- `tools/bin/verify_retire_drain.{bat,sh}` 用于 live cluster retire drain smoke。
- `tools/bin/verify_retire_drain.{bat,sh} --cycles N` 用于连续 retire drain smoke，
  每轮验证目标 app ready、stuck 为 0 且 LB decision watcher 推进。
- `tools/bin/verify_cellapp_rehome.{bat,sh}` 用于 live cluster CellApp crash /
  rehome smoke，验证目标 CellApp 消失、BSP leaf 迁走、BaseApp death notification、
  scheduled / restored / lost / timeout / pending restore watcher 和
  restore elapsed、BaseApp scheduled restore 来源拆分与幸存 CellApp restore
  source / failure watcher、LB
  `reason=cellapp-death` 决策；`--cycles N` 可连续 crash / rehome 多个当前持
  leaf 的 CellApp，`--min-restores N` 可按 `baseapp/cellapp_routes` 要求每轮
  真的发生 BaseApp-routed 实体 restore 调度，`--min-ghost-backup-restores N`
  可要求 Ghost backup source 也被实际命中，`--min-payload-restores N` 可要求
  Base cached payload source 被实际命中，`--min-promoted-restores N` 可要求
  Ghost→Real promotion 被实际命中；多轮最终 PASS 会输出聚合恢复量、来源拆分、
  失败计数和恢复耗时分布，`--summary-json PATH` 可把同一结果落成 JSON。
- `tests/unit/test_verify_cellapp_rehome.py` 覆盖 live verifier 对 BaseApp restore
  notification、scheduled/restored、payload / Ghost backup scheduled 来源拆分、
  lost、timeout、pending、elapsed 上限，以及 CellApp restore source / failure
  的健康判定。
- MVP Unity standalone + `tools/bin/run_mvp_unity_bots.bat` 已验证 1 bot 登录、
  auth、绑定 BaseApp、进入 4-leaf Space，且 LB watcher `pending_ack=0`。

报告 Phase 11 LB 行为工作前，至少跑与改动面匹配的 `test_cellappmgr*` /
`test_bsp_tree` / offload integration；涉及 Unity 可见行为时同步跑 MVP
Unity build 或 bot smoke。
