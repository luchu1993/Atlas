# Phase 13: 高可用 — Reviver + Manager Recovery

**Status:** 🚧 CellAppMgr HA 已落地到可验证 MVP；BaseAppMgr / DBAppMgr HA
尚未启动。当前代码支持 CellAppMgr 本地 Snapshot / Restore、周期性与 dirty
snapshot 文件、CellApp reattach geometry replay、`RequestCellAppState` 状态重拉，以及
带格式校验的 snapshot 文件、Reviver leader lock、direct heartbeat、
manager health audit、liveness / registry audit 和最小 Reviver cold-start /
abnormal-death restart；live fault-injection 已验证 CellAppMgr 重启后
存活 CellApp 自动重连并完成 reattach。
**前置依赖:** Phase 11（分布式空间完整可用）
**BigWorld 参考:** `server/reviver/`, `server/dbappmgr/`

## 目标

为 Atlas 集群添加 BigWorld 风格的 Manager 高可用能力。全局唯一 Manager
由 Reviver 监督；Manager 崩溃后由自身 Snapshot / Restore 恢复权威状态，
普通 App 的故障由对应 Manager 处理。

## 当前已落地能力

- **CellAppMgr snapshot**: `Snapshot()` / `Restore()` 覆盖 CellApp 表、Space BSP、
  leaf load profile（含 native tick 和 tick-cost buckets）、pending geometry
  broadcast、retire drain、next ids、per-space geometry version 和 freeze epoch。
- **restore semantic validation**: Restore 校验 `app_id` 唯一性、finite load、
  leaf host、pending geometry 和 retire drain 的跨表引用；语义不一致的
  snapshot 会被拒绝。
- **snapshot 文件**: `--snapshot-path` + `--snapshot-interval-ms` 周期写本地文件；
  文件使用 magic / version / payload size / checksum envelope，先写 tmp 再用
  平台原子替换；当前开发期 schema 持久化 `native_tick_us`、tick-cost buckets
  和 retire drain 状态；所有成功落盘都会递增 `snapshot_saves`，每次成功写
  新文件前保留上一份 `.bak`，主文件恢复失败时会回退到通过完整校验的 `.bak`，
  并单独计入 fallback restore；
  CellApp 注册、geometry 发布、deferred geometry ack / timeout、retire 标记 /
  handoff / drain 完成、CellApp death 和 reattach registry reconcile 会标记
  dirty snapshot，并在 tick 内按最多 1s 节流提前落盘；
  周期 snapshot 失败按 interval 节流重试，避免坏路径每 tick 重复刷日志；
  受控 shutdown 前会再 flush 一次；启动时 snapshot 缺失可跳过，主备
  snapshot 均损坏或不兼容时会失败启动。
- **HA watcher**: `cellappmgr/ha/snapshot_path`、`snapshot_bytes`、
  `snapshot_file_present`、`snapshot_file_bytes`、`snapshot_file_status`、
  `snapshot_file_topology_status`、
  `snapshot_backup_path`、`snapshot_backup_present`、`snapshot_backup_bytes`、
  `snapshot_backup_status`、`snapshot_backup_topology_status`、`snapshot_interval_ms`、
  `snapshot_last_save_attempt_age_ms`、`snapshot_last_save_age_ms`、`snapshot_last_save_path`、
  `snapshot_last_save_topology`、`snapshot_last_save_topology_pending_ack`、
  `snapshot_last_save_error`、`snapshot_dirty`、`snapshot_dirty_age_ms`、
  `snapshot_dirty_reason`、`snapshot_save_stale`、
  `snapshot_status`（含 `topology_present` / `topology_pending_ack` /
  `dirty` / `dirty_reason` / `error_detail`）、
  `snapshot_last_restore_source`、`snapshot_last_restore_attempt_age_ms`、
  `snapshot_last_restore_age_ms`、`snapshot_last_restore_path`、
  `snapshot_last_restore_topology`、`snapshot_last_restore_topology_pending_ack`、
  `snapshot_last_restore_error`、
  `snapshot_last_restore_primary_error`、`snapshot_restore_status`
  （含 `topology_present` / `topology_pending_ack` / `error_detail` /
  `primary_error_detail`）、`snapshot_saves`、`snapshot_restores`、
  `snapshot_fallback_restores`、`snapshot_save_failures`、`snapshot_restore_failures`、
  `snapshot_failures`、`snapshot_backup_skips`、`snapshot_max_bytes`、
  `snapshot_size_high_water_pct`、`restored_cellapps`、`reattach_pending`、
  `reattach_completed_count`、`reattach_completed`、`reattach_stuck`、
  `reattach_state`、`reattach_watchdog_ms` 和 `reattach_status`（含 `state` /
  `completed_count`）、`restore_gate_active`、`restore_gate_blocked_pending_geometry`
  和 `restore_gate_status`、`reattach_registry_audits`、
  `reattach_registry_last_missing`、`reattach_registry_last_blocked`、
  `reattach_registry_reconciled_total`、`reattach_registry_status`；
  snapshot file status 和
  file topology status 在失败时带有界 `error_detail`，便于定位 envelope、
  读文件或 dry-run restore 失败。
- **CellApp reattach**: restore 后 CellApp 重新注册保留原 `app_id`，mgr 重挂
  channel 并 replay `AddCellToSpace` / `UpdateGeometry`。存活 CellApp 会订阅
  CellAppMgr birth / death，mgr 重启后断开旧 channel、清空旧 `app_id` 并
  重新注册。同一 pid / 地址的 birth replay 会被忽略，避免重复断连重注册；
  同端口新 pid 仍会触发重连。
- **restore gate**: reattach pending 期间 CellAppMgr 冻结 LB tick、elastic grow、
  auto split / merge 和 retire drain 拓扑推进；pending `AddCellToSpaceAck` 未完成的
  Space 不再提前移动 BSP 边界。restore 携带的 pending geometry 若目标 CellApp
  尚未 reattach，不会通过 timeout fallback 发布 geometry。
- **状态重拉**: reattach replay 后发送 `RequestCellAppState`，CellApp 立即回传
  load、local cell 和 geometry version；CellAppMgr LB watcher 暴露 load report
  age / stale count，便于确认 reattach 后是否拿到新鲜样本。
- **不可达 host 防护**: 尚未 reattach 的 CellApp 不参与新 Space bootstrap、
  auto split 和 death rehome；其 `InformCellLoad` 与 deferred `AddCellToSpaceAck`
  会被忽略。
- **Reviver cold-start**: `--revive-cellappmgr-on-start` 可在 machined 中没有
  目标 CellAppMgr 时启动新进程。
- **Reviver restart**: machined abnormal death 通知后延迟重启 CellAppMgr，
  默认最多 3 次连续尝试；新 manager direct heartbeat ack 后重置连续重启预算。
  启动后未在 `revive-cellappmgr-launch-timeout-ms` 内注册会按失败重试；
  连续失败耗尽预算后通过 watcher 暴露明确报警状态，不再只依赖 `last_error`。
  `revive-restart-backoff-cap-ms` > 0 时,重启延迟会按 `attempts` 指数翻倍并被
  cap 截断,避免坏 exe 在固定延迟下迅速耗尽预算。
- **Reviver direct heartbeat**: 主 Reviver 会通过 CellAppMgr 二进制
  `HealthProbe` / `HealthProbeAck` 验证目标 manager 的 RUDP 控制面仍可响应；
  Ack 携带目标 mgr 的 game time、snapshot saves / failures / dirty / stale
  摘要，并通过 last ack age 和 `heartbeat_snapshot_status` 聚合 watcher 暴露
  heartbeat 与 HA snapshot 是否仍新鲜；
  Reviver 不依赖 machined watcher 也能观察 HA snapshot 状态。heartbeat 响应
  超时可独立配置，连续失败独立计数，不会被 watcher health 成功掩盖。
- **Reviver manager health**: 主 Reviver 会通过 machined watcher forwarding
  查询目标 CellAppMgr 的 `app/uptime_seconds`，确认 Manager 事件循环仍响应；
  pending watcher 超过 `revive-cellappmgr-manager-health-timeout-ms` 会被本地
  判定为超时，本地目标连续失败时会先终止旧 pid 再走重启路径。
- **Reviver liveness / registry audit**: 主 Reviver 会对本地目标检查 PID 是否
  仍存活，并周期性查询 machined registry；如果活跃 CellAppMgr 连续缺失，
  即使没有收到 death 通知也会走异常重启路径。
- **machined shutdown reason**: `atlas_tool shutdown <type[:name]> <reason>` 经
  machined 转发后会保留目标 reason；目标随后 deregister / disconnect /
  heartbeat timeout 时，listener death 通知继续携带同一 reason。Reviver 因此能
  区分 abnormal shutdown 和 reason 0 正常退出。
- **MachinedClient request timeout**: async registry query 和 watcher query 在
  machined 连接保持但请求无回复时会本地超时，避免上层 audit pending 卡死。
- **Reviver leader lock**: Reviver 启动后获取目标 CellAppMgr 的进程级
  leader lock；同机多实例只有持锁者会 cold-start / restart，未持锁实例保持
  standby。
- **Reviver 配置**: 支持目标 exe、name、internal port、snapshot path、
  snapshot interval、output path、update hertz、restart delay、max restarts、
  heartbeat timeout、manager health、registry audit 和 leader lock path；
  Reviver 使用 `--config` 启动时会把同一 config 文件传给新 CellAppMgr，并通过
  CLI 覆盖目标进程身份和端口。
- **Reviver watcher**: `reviver/cellappmgr/active`、`active_pid`、
  `active_generation`、`status`、`launched_pid`、`output_path`、`launch_pending`、
  `restart_attempts`、`launch_count`、`launch_failures`、`launch_timeouts`、`restart_limit_reached`、
  `restart_limit_hits`、`liveness_failures`、`health_checks`、`health_failures`、
  `manager_health_failures`、`manager_health_timeouts`、`heartbeat_sent`、
  `heartbeat_timeout_ms`、`heartbeat_acks`、`heartbeat_failures`、`heartbeat_timeouts`、
  `heartbeat_last_ack_age_ms`、`heartbeat_last_game_time`、`heartbeat_snapshot_saves`、
  `heartbeat_snapshot_failures`、`heartbeat_snapshot_dirty`、
  `heartbeat_snapshot_save_stale`、`heartbeat_snapshot_status`、
  `forced_terminations`、`registry_audits`、`registry_missing`、`last_error`、
  `reviver/leader/active`、`lock_path`、`acquire_count` 和 `acquire_failures`。
  `status` 可为 `standby`、`idle`、`querying`、`restart_scheduled`、`launching`、
  `active` 或 `restart_limited`。
- **live HA tooling**: `run_world_stress.py --with-cellappmgr-reviver` 可在
  world-stress 集群内启动 Reviver、CellAppMgr snapshot 和 leader lock；
  `--cellappmgr-reviver-count N` 会启动共享 snapshot / leader lock 的多 Reviver
  leader / standby 拓扑；
  `verify_cellappmgr_ha.{bat,sh}` 会注入 abnormal shutdown，并验证 generation、
  heartbeat、snapshot restore、Space / leaf topology 连续性、CellApp reattach
  收敛、Reviver leader lock 唯一 active leader 和重启后的稳定窗口；
  `--cycles` 可连续注入多轮异常接管，`--no-inject` 可用于非破坏性巡检当前
  Reviver 稳定性和 reattach pending / stuck 状态；高 churn 压测可用
  `--allow-topology-change` 显式跳过拓扑指纹一致性；`--summary-json PATH`
  可把接管轮次、PID / generation / launch delta、snapshot、reattach 和稳定窗口
  结果写成机器可读基线，并在 `parameters` 中记录本次 `--min-*`、`--max-*`
  和故障注入开关；已启用的 SLO / 拓扑 gate 会在 `gates` 中记录指标名、
  观察值、最小值或最大值以及通过状态，gate 未通过时也会先落盘；前置
  watcher、拓扑或收尾检查失败时也会写入 `current.run_healthy=false`、
  `failure_stage` / `failure_detail` 和 `run_health` gate；summary 同步记录
  `gate_count`、`gate_failures`、`run_health_checks`、`run_healthy` 和
  `run_unhealthy`；`run_failure_stage`、`current_failure_stages`、
  `cycle_failure_stages`、`reviver_failover_failure_stages` 和 `failure_stages`
  可直接按失败阶段归档；`first_failure_stage` 和 `first_failed_gate` 暴露首因；
  脚本失败输出也会带这两个首因字段；
  `failed_gate_names` / `failed_gates`、`overall_healthy` /
  `overall_success_rate` 标识失败 gate 明细和整次验证是否通过所有 gate，
  方便 CI 直接归档趋势；
  `--max-takeover-ms N` 可把 shutdown 到 fresh snapshot
  的接管耗时纳入生产 SLO 闸门；`--max-load-report-age-ms N` 可把 reattach
  后的 CellApp load 样本年龄纳入同一生产闸门，超龄会写入 `gates` 后失败；
  `--min-revivers N` 可要求
  同一验证拓扑具备 standby Reviver 并拒绝多 active leader；
  `--verify-reviver-failover` 会关闭
  当前 active Reviver leader，要求 standby 获取 leader lock、direct heartbeat
  重新变新鲜，并确认 CellAppMgr pid 与 standby `launch_count` 不变；
  `--max-reviver-failover-ms N` 会把 active Reviver 退出到 standby 健康接管的
  耗时纳入 SLO 闸门；`--min-post-failover-standbys N` 会要求 leader 故障后
  仍保留 N 个 standby Reviver；`--reviver-failover-cycles N` 可连续关闭当前
  active leader，验证多 Reviver 拓扑的级联接管能力；standby Reviver 必须保持
  `reviver/leader/active=false`、`status=standby`、无 pending launch，且未触发
  restart limit。
- **CellApp failure smoke**: 普通 CellApp 崩溃由 CellAppMgr rehome 和 BaseApp
  death restore 路径处理；`verify_cellapp_rehome.{bat,sh}` 会同时校验拓扑
  rehome、BaseApp restore scheduled / restored / lost / timeout / pending 收敛和
  restore elapsed，并要求幸存 CellApp 的 death-restore payload / Ghost backup
  来源计数分别覆盖 BaseApp 本轮 payload / Ghost backup scheduled restore，且
  empty / failure 不增长；`--cycles N` 可连续注入多轮 CellApp crash，
  `--min-target-entities N` 和 `--min-target-leaves N` 可要求每轮杀掉足够规模的
  CellApp，`--min-restores N` 可把验证从拓扑 smoke 升级为实体恢复基线，
  并引导目标选择命中持有足够 BaseApp route 的 CellApp。`--allow-no-baseapp`
  只适用于拓扑 smoke，不可与实体恢复规模或耗时闸门组合；
  `--min-ghost-backup-restores N`
  会按 BaseApp route 中的 Ghost backup candidate 选择目标，并把 Ghost backup
  fallback 覆盖纳入生产 HA 基线；`--min-payload-restores N` 会按 payload
  candidate 选择目标，并把 Base cached payload 恢复覆盖纳入同一基线。
  `--min-promoted-restores N` 会要求实际 death restore 至少发生 N 次
  Ghost→Real promotion。`--min-payload-restore-share`、
  `--min-ghost-backup-restore-share` 和 `--min-promoted-restore-share`
  可把多轮总恢复里的来源 / promote 占比纳入 CI 闸门；`--min-total-*`
  系列和 `--min-restore-latency-samples` 可约束多轮总恢复量和耗时样本数。
  多轮完成后会汇总 scheduled / restored、payload / Ghost backup 来源、
  empty / failure、promote、`restore_completion_rate`、
  `restore_source_coverage_rate`、payload / Ghost backup 期望覆盖率、
  payload / Ghost backup / promote 恢复占比，以及有实体恢复轮次的
  restore elapsed sample count / avg / p50 / p95 / max；`--summary-json PATH`
  可把恢复量、成功率、覆盖率和每轮指标写成 JSON，并在 `parameters` 中记录
  本次 `--min-*`、`--max-*`、目标规模、目标选择和 BaseApp 校验开关；启用的
  summary gate 会在 `gates` 中记录指标名、观察值、最小值和是否通过，供 CI /
  压测归档；若 summary gate 未通过，脚本仍会先写出该 JSON 再返回失败。

## 关键设计决策

### Reviver 只监督全局唯一 Manager

当前 Reviver 只覆盖 CellAppMgr。它订阅 machined birth / death，按进程名匹配
目标 CellAppMgr；启动时会先查询 machined 当前 registry，避免和已经存在的
CellAppMgr 发生重复 cold-start。同 pid / 地址的 birth 或 query replay 会被
幂等忽略，不会重置 heartbeat 状态或递增 active generation。持锁 Reviver
还会发送 direct heartbeat，并通过 `app/uptime_seconds` watcher 检查 Manager
响应性；对本地目标检查 PID liveness，并周期性查询 machined registry，弥补
listener death 通知丢失时的恢复路径。direct heartbeat 和 manager watcher
使用独立连续失败计数；
任一路径达到阈值时都会触发本地目标终止和重启。Machined 在同名注册时会
清理 PID 已死亡的 stale entry，避免旧 registry 阻塞新 Manager 注册。本地
目标 health failure、manager health watcher timeout 或 registry missing 达到
阈值时，Reviver 会终止旧 pid 后再重启，避免挂死进程继续占用 internal port。
Reviver 不尝试重建业务状态。
同机多 Reviver 通过 leader lock 选出唯一主动监督者；standby 实例保留进程
和 watcher，但不会启动或重启 Manager。

### Manager 自己恢复状态

CellAppMgr 是 BSP 权威，所以 snapshot 由 CellAppMgr 自己生成和消费。restore
出来的 CellApp 先处于待 reattach 状态；真实 CellApp 重新注册后才恢复 channel
和拓扑 replay。`cellappmgr/ha/reattach_status` 暴露恢复收敛状态；
`reattach_watchdog_ms` 超时后会把 pending host 标记为 stuck 并写 warning。
  这样避免把新 cell 分配给不可达 host 的同时，也能让运维判断旧 CellApp
是否全部回归或卡住。reattach 完成前 LB 和 pending geometry timeout 不会发布
新的权威 BSP 变化，避免恢复窗口内出现旧 CellApp 尚未建 cell 就被 offload 的
状态。CellAppMgr 会在 reattach pending 期间审计 machined CellApp registry；
已从 registry 消失且没有 leaf 的旧 host 会被清理，有可用 survivor 时会按
CellApp death 路径 rehome，缺少可接管目标时保持 restore gate closed 并暴露
blocked 计数。CellApp 侧用 machined birth / death 触发重连，所以 manager 重启
不要求同时重启全部 CellApp。

### 本地 snapshot 是当前 MVP 存储层

当前 snapshot 是单机本地文件，适合本地开发、单机 smoke 和最小 HA 验证。
文件格式已有 envelope 校验、schema 版本、原子替换、`.bak` 回退、
semantic validation、dirty topology flush 和当前 load profile 持久化；
生产形态仍需要共享存储、WAL 或外部一致性层来避免 Reviver 与 Manager 跑在
不同机器时看不到最新 snapshot。

## 交接状态

CellAppMgr HA 当前可以作为同机 HA MVP 和 CI / 压测基线继续使用。核心路径是
Reviver 监督全局唯一 CellAppMgr，CellAppMgr 自己持久化和恢复 BSP / CellApp
权威状态；CellApp 存活时通过 reattach、topology replay 和状态重拉收敛。
故障注入脚本已把接管轮次、最终 current 状态、Reviver failover、SLO gate、
失败首因和每轮 `healthy` / `failure_stages` 都写入 summary JSON，适合后续
CI 直接归档。

后续接手时，先用脚本级回归确认 summary schema 没漂移，再跑 live fault
injection。脚本级最小回归：

```powershell
python tests\unit\test_verify_cellappmgr_ha.py
python -m py_compile `
  tools\cluster_control\verify_cellappmgr_ha.py `
  tests\unit\test_verify_cellappmgr_ha.py
python tools\cluster_control\verify_cellappmgr_ha.py --help
```

live 回归优先覆盖 CellAppMgr abnormal shutdown、`--cycles` 多轮接管、
`--no-inject` 巡检和 `--verify-reviver-failover` standby 接管。

## 当前边界

- Reviver 只支持 CellAppMgr；BaseAppMgr / DBAppMgr 还没有 Snapshot / Restore。
- Reviver leader lock 现在支持两种模式:
  - `local`(默认):per-host 文件锁,跟之前一样,单机有效;
  - `machined`:lease 由 machined 持有,跨机 Reviver 可以竞争同一 key。
    machined 在 `OnTickComplete` 周期 prune 过期 lease,disconnect 时
    drop 当前 channel 持有的全部 lease。Reviver 通过
    `--revive-leader-lock-mode machined` 切到分布式模式;
    `--revive-leader-lock-ttl-ms` (默认 8s)和 `--revive-leader-lock-renew-ms`
    (默认 3s)控制 renew 节奏,`--revive-leader-lock-failure-threshold` 控制
    连续 renew 失败后主动放弃 leadership 的次数。
  跨机器集群仍需要 machined 本身的可用性 — machined HA 不在 Phase 13
  范围内,如果 machined 单点掉电,所有 Reviver 都失去 leader,但只要
  machined 恢复,Reviver 会自动重新竞争。
- 异常检测已有 machined death 通知、CellAppMgr direct heartbeat、manager
  watcher health、Reviver 本机 PID liveness 和 registry audit；跨机器误判
  防护仍依赖外部 leader lock / 共享状态方案。
- snapshot 是周期 + dirty flush 文件，不是 WAL；dirty 窗口内 crash 或本地
  save 失败后的状态变化仍可能需要靠 CellApp reattach 和 load 重拉收敛。
- restore 后 reattach 卡住的 CellApp 不会被自动清理：mgr 只暴露 `reattach_stuck`
  / `reattach_status` watcher 和 warning 日志，依赖运维通过
  `atlas_tool shutdown cellapp:<name> <reason>` 手工介入；新 Space 分配、auto
  split、death rehome 都会跳过 stuck host，所以 stuck 不会污染拓扑决策。
- `restored_from_snapshot` 是 sticky 标记：CellApp 被 Restore 重建后保留 true，
  reattach 完成也不重置，只在 `OnCellAppDeath` 才清掉。`restored_cellapps`
  watcher 因此报告"自上次 restore 以来此 CellApp 仍在", `reattach_completed_count`
  反映其中已完成 reattach 的数量，supporting verify 脚本的 `require_restored` 闸门。
- snapshot 主文件 envelope 校验失败时不会用坏内容覆盖 `.bak`，但会推进 `.bak`
  落后一代；通过 `cellappmgr/ha/snapshot_backup_skips` 暴露累计跳过次数，运维
  发现持续增长时需要检查磁盘 / 文件系统而不是 mgr 自身。
- DBAppMgr 多 DBApp、分片迁移和 DBApp 故障转移仍未实现。
- BaseApp crash 后的客户端 session resume 尚未实现；当前仍走重新登录路径。
- CellApp 实体恢复已有 live smoke、多轮目标规模、恢复量、恢复耗时和
  payload / Ghost backup / promote 覆盖率基线；更大规模跨机器 fault-injection
  矩阵仍需补齐。

## BaseAppMgr HA(B1-B4 已落地)

- **Snapshot**:`Snapshot()` / `Restore()` 持久化 BaseApp 表(internal/external addr、
  app_id、is_ready、is_retiring)、`next_app_id_`、`global_bases_` 注册表和
  `dbid_affinity_` 表(以保存时刻为基准的 age-relative,Restore 后 PruneExpired
  仍能匹配 TTL)。复用 CellAppMgr 的 envelope / `.bak` / `AtomicReplaceFile`
  机制(独立 magic `'BMG1'`,version 1)。
- **Reattach**:Restore 后 BaseApp 进入 `needs_reattach=true` 状态,
  `OnRegisterBaseapp` 识别 needs_reattach entry 走 reattach 分支(保留 app_id,
  替换 channel,清 needs_reattach)。`IsAllocationCandidate` 拒绝 needs_reattach
  host 防止 LoginApp 把新客户端分配到 unreachable BaseApp。
  `restored_from_snapshot` 与 CellAppMgr 一致是 sticky 标记(只在 BaseApp 死亡
  时清),`restored_baseapps` watcher 因此报告"自上次 restore 以来此 BaseApp 仍在"。
- **Reattach watchdog**:`AuditReattachWatchdog` 在 `OnTickComplete` 检查
  stuck BaseApp;`baseappmgr/ha/reattach_watchdog_ms` 是 ReadWrite ServerAppOption
  (默认 30s),verify 脚本可以通过 `atlas_tool set-watch` 缩短窗口。
- **Watcher**:`baseappmgr/ha/snapshot_path`、`snapshot_saves`、`snapshot_restores`、
  `snapshot_fallback_restores`、`snapshot_save_failures`、`snapshot_restore_failures`、
  `snapshot_failures`、`snapshot_backup_skips`、`snapshot_max_bytes`、
  `snapshot_size_high_water_pct`、`snapshot_dirty`、`snapshot_dirty_age_ms`、
  `snapshot_dirty_reason`、`snapshot_save_stale`、`snapshot_status`、
  `snapshot_last_save_path`、`snapshot_last_save_error`、`snapshot_file_status`、
  `snapshot_backup_status`、`snapshot_last_restore_source`、
  `snapshot_last_restore_error`、`snapshot_restore_status`、`restored_baseapps`、
  `reattach_pending`、`reattach_completed_count`、`reattach_completed`、
  `reattach_stuck`、`reattach_state`、`reattach_status`、`reattach_watchdog_ms`。
- **Reviver 扩展**:Reviver 重构为 multi-target,持有 `cellappmgr_target_` 和
  `baseappmgr_target_`,各自独立 leader lock、heartbeat、launch 预算、watcher
  路径。BaseAppMgr 加 `HealthProbe` / `HealthProbeAck` 消息(message id 6020/
  6021),Reviver 通过 `baseappmgr::HealthProbe` 校验 manager RUDP 控制面响应。
  watcher 路径 `reviver/baseappmgr/*` 与 CellAppMgr 对齐;CellAppMgr 继续走
  `reviver/cellappmgr/*` 和 `reviver/leader/*`(legacy alias)。
- **ServerConfig**:`revive_baseappmgr_*`(exe、name、internal_port、snapshot_path、
  output_path、snapshot_interval_ms、update_hertz、launch_timeout_ms、on_start、
  leader_lock_path)平行于 `revive_cellappmgr_*`,均支持 CLI 和 `reviver.baseappmgr`
  JSON 段。目标 enabled 当 on_start 或 exe/port 任一已配置 — 未配置时该 target
  完全 silent,旧的 single-target Reviver 行为保持不变。
- **verify_baseappmgr_ha**:`tools/cluster_control/verify_baseappmgr_ha.py` +
  `tools/bin/verify_baseappmgr_ha.{bat,sh}` 提供 `--no-inject` 巡检和
  `--cycles N` 异常重启注入。校验 snapshot saves/restores、save_failures=0、
  reattach state=complete、Reviver active_pid 推进到新 manager、heartbeat
  acks > 0。`--summary-json` 输出与 verify_cellappmgr_ha 风格一致;
  `--max-takeover-ms` SLO gate 限制 shutdown→fresh-snapshot 耗时。
- 当前边界:verify 脚本未实现 Reviver leader failover 跨机器场景;snapshot
  仍是本地文件;BaseAppMgr 死亡时 BaseApp 自身的 entity restore 走 Phase 13
  原有 CellApp death restore 路径(BaseApp 不受 BaseAppMgr 重启直接影响)。

## 后续工作

1. 为 DBAppMgr 定义多 DBApp registry、分片策略、故障转移和 pending request 恢复。
2. 评估 CellAppMgr / BaseAppMgr snapshot 的共享存储 / WAL 方案,进一步缩小
   本地文件丢失窗口。
3. 扩大 Manager HA live cluster fault-injection 矩阵,覆盖跨机器 Reviver
   (machined-mode lease)、snapshot 共享存储和更大规模 continued LB,并把
   BaseAppMgr live takeover 纳入 CI baseline。

## 验证基线

- `tests/unit/test_cellappmgr.cpp` 覆盖 CellAppMgr Snapshot / Restore、
  snapshot 文件 round-trip、native tick / tick-cost profile 和 retire drain
  状态持久化、主文件损坏或缺失时 `.bak` 回退、last save / fallback restore /
  last restore watcher、snapshot freshness / stale watcher、restore attempt / success age
  watcher、restore status summary、dirty topology flush、save / restore failure counter、
  版本 / checksum / 损坏文件拒绝、
  周期失败重试节流、shutdown snapshot flush、semantic validation、reattach replay、
  恢复收敛 / stuck watcher、
  未 reattach host 防护和 state request。
- `tests/unit/test_filesystem.cpp` 覆盖平台 `AtomicReplaceFile` 替换已有目标文件。
- `tests/unit/test_cellapp_handlers.cpp` 覆盖 `RequestCellAppState` 触发立即
  `InformCellLoad`，并带 local geometry version。
- `tests/unit/test_cellapp_handlers.cpp` 覆盖 CellAppMgr birth replay 判定：
  同 pid / 地址不重连，新 pid、新地址或旧 channel down 后会重连。
- `tests/unit/test_server_config.cpp` 覆盖 snapshot / Reviver CLI 与 JSON 配置解析，
  包括 revived CellAppMgr output path。
- `tests/integration/test_cellappmgr_process.cpp` 覆盖 MachinedClient async query
  和 watcher query 对 silent machined 连接的本地 timeout。
- `tests/integration/test_machined_registration.cpp` 覆盖 machined 转发 shutdown
  后，目标 deregister 产生的 death notification 保留原 reason。
- `tests/integration/test_cellappmgr_process.cpp` 覆盖真实 Reviver cold-start
  CellAppMgr、异常终止后重启到同一 internal port，以及同机多 Reviver
  leader lock 防止重复 cold-start、late Reviver attach 不重启已有 mgr、
  Reviver 启动前目标已死时拉起新 mgr、leader 退出后 standby 接管不重启已有
  mgr、目标 watcher 可响应但 direct heartbeat 不回包时触发 forced termination
  与重启、连续 no-ack manager 达到 restart limit 后停止重启、manager health
  watcher pending timeout、launched process 不注册时重试到 restart limit；
  同时验证 Reviver `status`、`launch_pending`、direct heartbeat、heartbeat
  snapshot 摘要、manager health 和 registry audit watcher，并覆盖 revived
  CellAppMgr 继承配置文件中的 LB 权重。
- `tools/bin/verify_cellappmgr_ha.{bat,sh}` 覆盖 live cluster fault-injection：
  abnormal shutdown CellAppMgr，等待 Reviver 重启，并验证 snapshot restore、
  snapshot save / restore watcher 健康、primary snapshot file envelope / checksum readiness
  （失败时暴露有界 `error_detail`）、
  primary snapshot file dry-run restore topology readiness（失败时暴露有界
  `error_detail`），且健康或允许缺失状态必须为 `error_detail=none`、
  snapshot freshness / stale 状态、
  `.bak` fallback envelope / checksum readiness（默认要求 `state=ready valid=1`）、
  `.bak` fallback dry-run restore topology readiness、
  重启前后 `cellappmgr/lb/spaces` 的 Space / leaf topology fingerprint 一致性、
  kill 前最后一次成功落盘 snapshot 的 topology fingerprint 与当前 live topology
  一致性且 pending ack 为 0、
  restore 成功后 `snapshot_last_restore_topology` 与预期 topology fingerprint 一致性且
  pending ack 为 0、
  restore attempt / success age、restore status summary、
  fallback primary error、snapshot failure 聚合计数一致性、Reviver active generation
  递增、leader lock 只有一个 active Reviver、direct heartbeat last ack age、
  heartbeat snapshot 摘要且 failure counter 不增长、manager health、
  CellApp 自动重连、reattach completed、load report fresh / age SLO、
  restored / pending / completed_count / state watcher 与 status summary 字段一致、
  stuck watcher 和 revived CellAppMgr 独立日志；
  snapshot save 校验允许历史累计失败，但会拒绝验证窗口内新增 save failure
  或 dirty snapshot 未清空。
  每轮 restore 收敛后还会等待 revived CellAppMgr 重新写出 fresh snapshot，
  并要求 last save age 不大于 last restore age，避免只验证读取成功而漏掉
  新 mgr 落盘失败；最终稳定窗口会持续校验 Reviver heartbeat snapshot
  没有变脏、变 stale 或新增 failure。
  `--no-inject` 也会校验 snapshot failure 聚合计数、当前拓扑可解析且无
  pending ack，以及 reattach / restore gate / registry watcher 和 summary
  一致且没有 pending、stuck 或 registry blocked。
  收敛后还会等待稳定窗口，确认没有二次 restart、heartbeat timeout 或
  forced termination。
  `--cycles` 可对同一 live cluster 连续执行多轮异常
  接管；`--no-inject` 模式复用同一稳定窗口做非破坏性巡检；
  `--summary-json` 可在检查完成后落盘机器可读接管摘要，并把已启用的
  SLO / 拓扑 gate 写入 `gates`；
  `--max-takeover-ms` 可限制每轮 shutdown 到 revived mgr 写出 fresh snapshot 的
  takeover elapsed；`--min-revivers` 可把 standby Reviver 覆盖纳入 HA 基线。
  `--verify-reviver-failover` 会先关闭 active Reviver leader，等待 standby 接管
  同一个 CellAppMgr，并拒绝 CellAppMgr pid 变化、standby `launch_count` 增长、
  heartbeat 未推进或多 active leader；`--max-reviver-failover-ms` 可限制
  Reviver leader failover 到 standby heartbeat / health 收敛的耗时，并写入
  summary JSON 的 `reviver_failovers` 与 `reviver_failover_*` 汇总字段。
  若 Reviver leader failover 未收敛，也会先写入不健康的 `reviver_failovers[]`
  明细，并以 `reviver_failover_health` gate 失败落盘。
  `--min-post-failover-standbys` 可把 leader 故障后的剩余 standby 冗余纳入
  生产基线，summary 会记录 `min_surviving_revivers` 和
  `min_post_failover_standbys`。`--reviver-failover-cycles` 会连续注入多轮
  Reviver leader 故障，summary 中每个 `reviver_failovers` 明细都带有 `cycle`。
  验证还会检查 standby Reviver 的 watcher 健康，并把当前
  `reviver_standby_health_status` 写入 summary；`current.reviver_topology` 提供
  最终重新读取的 leader、registered / active / standby 数量和 standby health
  的结构化字段；
  `current.reviver_topology.standby_health` 逐个记录 standby Reviver 的 pid、
  leader-active 状态、launch pending、restart limit 和健康结果；
  `current.stability_healthy` 记录最终稳定窗口是否通过，若不健康会以
  `stability_health` gate 失败落盘；
  若最终或 `--no-inject` 巡检中的 standby watcher 不健康，会以
  `reviver_standby_health` gate 失败落盘；
  `current.recovery` 结构化记录 reattach、restore gate、reattach registry 和
  load report 的最终基础健康状态，若最终或 `--no-inject` 巡检中的 recovery
  不健康，会以 `recovery_health` gate 失败落盘；
  `current.load_report` 记录每个 CellApp 的
  load age、stale 状态、over-age 状态和 summary 可直接读取的基础健康布尔值；
  `--max-load-report-age-ms` 的 summary gate 会取每轮接管与最终
  `current.load_report` 的最大 load age，避免最终收敛状态漏检。
  注入模式下，每个 `cycles[]` 明细也会写入同样的 `recovery` /
  `load_report`、per-cycle `healthy` 和 `failure_stages`，便于定位具体故障轮次；
  若任一轮 takeover、stability、recovery 或 load report 不健康，会分别以
  `cycle_takeover_health`、
  `cycle_stability_health`、`cycle_recovery_health` 或
  `cycle_load_report_health` gate 失败落盘；
  summary 会聚合 successful / failed cycle 数、takeover、stability、
  recovery / load report 健康检查数、健康 / 不健康数、load report 记录数、
  最大 load age 以及 stale / over-age app 总数。
- MVP smoke 已验证 CellAppMgr / CellApp LB 路径在 Reviver 相关改动后仍可登录、
  auth、绑定客户端并进入 4-leaf Space。
- `tools/bin/verify_cellapp_rehome.{bat,sh}` 覆盖 CellApp crash app-level HA smoke：
  death rehome 后要求 BaseApp restore scheduled 被 restored 覆盖，lost / timeout
  不增长，pending restore 归零，BaseApp scheduled restore 来源拆分与幸存
  CellApp death-restore source 一致；可用 `--max-restore-ms` 约束恢复耗时、
  `--min-target-entities` / `--min-target-leaves` 约束被杀 CellApp 的实体和
  BSP leaf 覆盖量、`--min-restores` 按 `baseapp/cellapp_routes` 约束恢复规模、
  `--min-ghost-backup-restores` 约束 Ghost backup fallback 覆盖，
  `--min-payload-restores` 约束 Base cached payload 覆盖，
  `--min-promoted-restores` 约束 Ghost→Real promotion 覆盖，并可用
  `--min-total-*`、`--min-restore-latency-samples` 和
  `--min-*-restore-share` 约束总恢复量、耗时样本数和来源 / promote 占比；
  多轮模式会输出
  聚合恢复量、来源拆分、失败计数、目标 leaf / entity / route 覆盖量、
  `restore_completion_rate`、`restore_source_coverage_rate`、
  `payload_expected_coverage_rate`、`ghost_backup_expected_coverage_rate`、
  `payload_restore_share`、`ghost_backup_restore_share`、
  `promoted_restore_share` 和恢复耗时分布，并可用 `--summary-json` 落盘
  机器可读结果；`parameters` 会记录本次门槛参数、目标规模和开关，`gates`
  会记录已启用 summary gate 的指标名、观察值、最小值和通过状态，summary gate
  未通过时也会先落盘；每轮 JSON 明细结构化记录目标 CellApp 名称、app_id、
  pid、地址、leaf 数、实体数和 BaseApp route / payload / Ghost backup candidate 数。
