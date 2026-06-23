# Atlas 全链路压测说明

> 更新时间: 2026-05-24
> 适用范围: LoginApp / BaseApp / DBApp / BaseAppMgr / CellAppMgr / CellApp 的端到端登录、世界态进入、cell RPC、AoI、短线重登压测

## 1. 目标

`world_stress` 是 `login_stress` 的超集，覆盖从客户端握手到 cell 侧 CLR 派发的整条链路：

```
client
   │
   ├── LoginRequest     → LoginApp           (认证)
   ├── Authenticate     → BaseApp            (账户实体材化)
   ├── ClientBaseRpc    → Account.SelectAvatar (脚本创建 StressAvatar)
   │                       ├── CreateBaseEntity native API
   │                       │   ├── RestoreEntity (C# 实例化)
   │                       │   └── CreateCellEntity → CellApp  (cell 实体上，尚无 witness)
   │                       ├── GiveClientTo(avatar)
   │                       │   └── BaseApp::BindClient → cellapp::EnableWitness
   │                       │         (witness 用 CellAppConfig 默认 500 m / 5 m)
   │                       └── avatar.SetAoIRadius(50, 5)
   │                             └── cellapp::SetAoIRadius  (压测收紧到 ±50 m 带)
   ├── ← EntityTransferred (客户端切换到 StressAvatar)
   ├── ← CellReady         (cell 已绑定，安全发 cell RPC)
   ├── ClientCellRpc → StressAvatar.Echo  (cell 侧 RPC 回环)
   │     └── Client.EchoReply → SelfRpcFromCell → client
   ├── ClientCellRpc → StressAvatar.ReportPos (位置更新 + AoI 广播)
   │     └── Position 属性 → 邻居收 kEntityPositionUpdate
   └── Disconnect / shortline / reconnect
         └── OnExternalClientDisconnect
               ├── UnbindClient → cellapp::DisableWitness  (witness 立即释放)
               └── FinalizeForceLogoff → DestroyCellEntity
```

## 2. 工具与脚本

### 2.1 集群拉起

- `tools/bin/run_world_stress.{bat,sh}` — 完整 stress driver（启动集群 +
  跑客户端 + 收日志 + 审计）的薄 wrapper，实际逻辑在
  `tools/cluster_control/run_world_stress.py`。
- `tools/bin/verify_retire_drain.{bat,sh}` — live cluster retire-drain 验证；
  通过 `atlas_tool set-watch` 标记 retiring CellApp，并轮询 CellAppMgr watcher
  直到 owned/drain/pending 清零且 `ready=1`；`--cycles N` 可连续执行多轮，
  每轮都会确认 LB decision watcher 推进。
- `tools/bin/verify_cellapp_rehome.{bat,sh}` — live cluster CellApp crash/rehome
  验证；通过 machined abnormal shutdown 一个持 leaf 的 CellApp，并轮询
  CellAppMgr / BaseApp watcher 直到 BSP leaf 全部 rehome。
- `tools/bin/verify_cellappmgr_ha.{bat,sh}` — live cluster CellAppMgr HA 验证；
  通过 machined 注入 abnormal shutdown，等待 Reviver 重启 mgr，并验证 snapshot
  restore、direct heartbeat 和 CellApp reattach 收敛。
- `tools/bin/run_cluster.sh` — Linux/macOS 只起集群，不跑客户端（preset wrapper
  → `run_world_stress --clients 0 --keep-cluster`）。Windows 侧可直接使用
  `tools/bin/run_world_stress.bat`;MVP Unity 调试可用
  `tools/bin/run_mvp_cluster.bat`。

相对 `run_login_stress.py` 的增量：
- 启动 CellAppMgr + N 个 CellApp
- 可选启动 Reviver 监督 CellAppMgr，并为 CellAppMgr 配置 HA snapshot
- BaseApp / CellApp 的 `--assembly` 指向 `samples/stress/Atlas.StressTest.{Base,Cell}`
- 新参数：`--cellapp-count`、`--space-count`、`--rpc-rate-hz`、`--move-rate-hz`

### 2.2 压测客户端

- `src/tools/world_stress/main.cc` — 继承 login_stress 的 Session 状态机

## 3. 环境前提

- 构建目录：`build/debug`（默认，可用 `--build-dir` 覆盖）
- 以下程序必须存在：
  - `machined` / `atlas_loginapp` / `atlas_baseapp` / `atlas_baseappmgr` / `atlas_dbapp`
  - `atlas_cellapp` / `atlas_cellappmgr`
  - `atlas_reviver`（仅 `--with-cellappmgr-reviver` 时需要）
  - `atlas_tool`
  - `world_stress`
- C# Runtime：
  - `runtime/atlas_server.runtimeconfig.json`
  - `bin/<build>/Atlas.StressTest.Base.dll`
  - `bin/<build>/Atlas.StressTest.Cell.dll`
  - `bin/<build>/Atlas.ClientSample.dll`（仅 `--script-clients`）
  - `run_world_stress` 会从实际传给 BaseApp 的 `Atlas.StressTest.Base.dll`
    读取 entity-def digest；若登录出现 `def_mismatch`，先重新构建 stress C# 产物。
- 端口段（默认）：
  - 20013 LoginApp external
  - 20018 machined
  - 21001+ BaseApp internal
  - 22001+ BaseApp external
  - 23001 BaseAppMgr
  - 24001 DBApp
  - 25001 CellAppMgr
  - 26001+ CellApp
  - 27001 Reviver（仅 `--with-cellappmgr-reviver` 时使用）

## 4. 推荐执行方式

### 4.1 P1 集群启动自检（不跑客户端）

```powershell
tools\bin\run_world_stress.bat `
  --clients 0 `
  --duration-sec 10
```

期望：7 个进程都注册到 machined，10 秒 hold 无错误，优雅退出。

需要保持集群供手动客户端连接时：

```powershell
tools\bin\run_world_stress.bat --clients 0 --keep-cluster
```

### 4.2 P2 最小活体（端到端闭环）

```powershell
tools\bin\run_world_stress.bat `
  --clients 1 `
  --account-pool 1 `
  --duration-sec 6 `
  --ramp-per-sec 1 `
  --hold-min-ms 2500 --hold-max-ms 2500 `
  --shortline-pct 0 `
  --rpc-rate-hz 0 --move-rate-hz 0
```

验证点：
- `login_started / login_success / auth_success`：匹配
- `entity_transferred` > 0 且等于 `cell_ready`
- 服务端日志有 `Account.SelectAvatar(index=...)` 和 `SelectAvatar: created StressAvatar`

### 4.3 P3 常规规模 + 空间分布 + 双 CellApp

```powershell
tools\bin\run_world_stress.bat `
  --clients 200 `
  --account-pool 200 `
  --duration-sec 30 `
  --ramp-per-sec 100 `
  --hold-min-ms 15000 --hold-max-ms 15000 `
  --shortline-pct 0 `
  --rpc-rate-hz 2 --move-rate-hz 10 `
  --space-count 4 --cellapp-count 2 `
  --login-rate-limit-trusted-cidr 127.0.0.0/8 `
  --login-rate-limit-global 10000
```

可持续基线（截至 2026-04-21 的测量值）：
- `entity_transferred / cell_ready`: 1:1
- `move_sent / fail`: ~54k / 0
- `echo_rtt_p50 / p95 / p99`: ≈ 1.3 / 2.2 / 6.2 ms
- `auth_latency_p95`: ~49 ms
- 所有失败计数 = 0
- 总 "no cell channel" drop = 0

### 4.4 LB retire-drain 验证

先保留一个有多 Space / 多 CellApp 的集群：

```powershell
tools\bin\run_world_stress.bat `
  --clients 200 `
  --account-pool 200 `
  --duration-sec 30 `
  --ramp-per-sec 100 `
  --hold-min-ms 15000 --hold-max-ms 15000 `
  --shortline-pct 0 `
  --rpc-rate-hz 2 --move-rate-hz 10 `
  --space-count 4 --cellapp-count 4 `
  --login-rate-limit-trusted-cidr 127.0.0.0/8 `
  --login-rate-limit-global 10000 `
  --keep-cluster
```

客户端压测完成且集群仍在运行后，执行：

```powershell
tools\bin\verify_retire_drain.bat --min-spaces 4 --timeout-sec 120
```

连续多轮：

```powershell
tools\bin\verify_retire_drain.bat --min-spaces 4 --timeout-sec 120 --cycles 3
```

Linux / macOS：

```bash
tools/bin/verify_retire_drain.sh --min-spaces 4 --timeout-sec 120
```

验收：脚本打印 `PASS`，`cellappmgr/lb/retire/status` 中目标 app 的
`owned=0 drains=0 pending=0 ready=1 stuck=0`，且
`cellappmgr/lb/retire/stuck_count == 0`；每轮 `decision_count` 必须增长。
未显式传 `--target-app-id` 时，脚本会选择当前持有 BSP leaf 的 CellApp；
显式目标若没有持有 leaf 会失败，避免空退役被误判为通过。

当前最小实测：8 clients / 2 Spaces / 3 CellApps 的 live 集群上，
`world_stress` 得到 `echo_sent=81`、`echo_received=81`、`echo_loss=0`；
随后 `verify_retire_drain.bat --min-cellapps 3 --min-spaces 2` 通过，目标 app
从 2 个 leaf drain 到 `owned=0 drains=0 pending=0 ready=1 stuck=0`。
连续 retire 实测：同规模集群上 `world_stress` 得到 `echo_sent=113`、
`echo_received=113`、`echo_loss=0`；随后
`verify_retire_drain.bat --min-cellapps 3 --min-spaces 2 --cycles 2` 通过，
两轮都收敛到 `ready=1 stuck=0`，`decision_count` 分别从 `2→6`、`8→10`。

### 4.5 CellApp crash/rehome 验证

保留一个至少 2 个 CellApp、1 个 Space 的 live 集群后执行：

```powershell
tools\bin\verify_cellapp_rehome.bat --min-cellapps 3 --min-spaces 2 --timeout-sec 120
```

Linux / macOS：

```bash
tools/bin/verify_cellapp_rehome.sh --min-cellapps 3 --min-spaces 2 --timeout-sec 120
```

连续故障验证可加 `--cycles N`，脚本每轮都会重新选择当前持 leaf 最多的
CellApp；多轮完成后会汇总 scheduled / restored、payload / Ghost backup
来源、empty / failure、promote、目标 leaf / entity / route 覆盖量、
`restore_completion_rate`、`restore_source_coverage_rate`、
`payload_expected_coverage_rate`、`ghost_backup_expected_coverage_rate`、
`payload_restore_share`、`ghost_backup_restore_share`、
`promoted_restore_share`，以及有实体恢复轮次的 restore elapsed sample count /
avg / p50 / p95 / max。
加 `--summary-json PATH` 可把同一批恢复指标写成 JSON，且每轮结构化记录
目标 CellApp 名称、app_id、pid、地址、leaf 数、实体数和 BaseApp route /
payload / Ghost backup candidate 数，便于 CI / 压测归档；summary 里的覆盖率
字段可直接作为生产 HA 基线趋势，`parameters` 会记录本次 `--min-*`、
`--max-*`、目标规模、目标选择和 BaseApp 校验开关；`gates` 会记录每个已启用
summary gate 的指标名、观察值、最小值和通过状态；若 summary gate 未通过，
脚本仍会先写出该 JSON 再返回失败。
用 `--max-restore-ms N` 可以把本轮实体恢复耗时也纳入闸门。要把
验证升级为大规模实体恢复基线而非纯拓扑 smoke，加 `--min-target-entities N`
和 `--min-target-leaves N`，要求每轮杀掉至少持有 N 个实体 / BSP leaf 的
CellApp；加 `--min-restores N` 可要求每轮至少调度 N 个 BaseApp death restore。
自动目标选择会优先杀当前满足目标规模和恢复候选门槛的 CellApp，固定
`--target-*` 不满足目标规模或路由数时会提前失败。
要约束多轮聚合恢复量，可加 `--min-total-scheduled-restores N`、
`--min-total-payload-restores N`、`--min-total-ghost-backup-restores N`、
`--min-total-promoted-restores N` 或 `--min-restore-latency-samples N`。
`--allow-no-baseapp` 只用于拓扑 smoke，不能和 `--min-restores`、
`--min-ghost-backup-restores`、`--min-payload-restores`、
`--min-promoted-restores`、`--min-total-*`、`--min-*-restore-share` 或
`--max-restore-ms` 组合。
要强制覆盖 Base cached payload 恢复，可加 `--min-payload-restores N`；
要强制覆盖 Ghost backup fallback，可加 `--min-ghost-backup-restores N`，
要求每轮至少 N 个 death restore 由 Ghost backup blob 恢复。
要强制覆盖 Ghost→Real promotion，可加
`--min-promoted-restores N`；该闸门依赖测试拓扑已产生可接管 Ghost。
要把来源占比纳入多轮生产基线，可加 `--min-payload-restore-share R`、
`--min-ghost-backup-restore-share R` 或 `--min-promoted-restore-share R`，
`R` 是 0 到 1 之间的小数；脚本会在全部轮次通过后用聚合 summary 校验。

验收：脚本打印 `PASS`，目标 CellApp 从 machined registry 消失，
`cellappmgr/cellapp_count` 减 1，`cellappmgr/lb/spaces` 不再有目标 app 的 leaf，
`cellappmgr/lb/decision_count` 增长，`last_decision` 包含
`reason=cellapp-death`，且每个 BaseApp 的
`baseapp/cellapp_death_notifications_total` 增长；`--min-restores` 的目标选择
使用 `baseapp/cellapp_routes`，只把 BaseApp 已绑定 Cell 的实体算入恢复基线。
同一 watcher 还暴露 `payload_candidates` / `ghost_backup_candidates`，
`--min-payload-restores` 会优先选择有足够 payload 候选的目标，
`--min-ghost-backup-restores` 会优先选择有足够 Ghost backup 候选的目标。
若本轮调度了实体恢复，
`baseapp/cellapp_death_restore_scheduled_total` 的增量必须被
`baseapp/cellapp_death_restored_total` 覆盖，`lost` / `timeout` 不能增长，
`baseapp/cellapp_death_pending_restores` 必须归零；开启 `--max-restore-ms`
时，BaseApp 的 last / max restore elapsed watcher 也必须在阈值内。幸存
CellApp 的 death-restore payload / Ghost backup source 增量必须分别覆盖
BaseApp 本轮 payload / Ghost backup scheduled restore，death-restore empty /
failure 计数不能增长。
开启 `--min-restores` 时，本轮 scheduled restore 增量还必须达到阈值。
开启 `--min-payload-restores` 时，payload source 增量也必须达到阈值。
开启 `--min-ghost-backup-restores` 时，Ghost backup source 增量也必须达到阈值。
开启 `--min-promoted-restores` 时，Ghost→Real promotion 增量也必须达到阈值。

当前最小实测：3 CellApps / 2 Spaces live 集群上，脚本杀掉
`cellapp_02 app_id=3`，目标从 2 个 leaf rehome 到 0 个，`decision_count`
从 `4→6`，最后一次决策为 `action=rehome reason=cellapp-death`。

### 4.6 CellAppMgr HA 验证

先保留一个带 Reviver 的 live 集群：

```powershell
tools\bin\run_world_stress.bat `
  --clients 8 `
  --account-pool 8 `
  --duration-sec 20 `
  --ramp-per-sec 4 `
  --hold-min-ms 15000 --hold-max-ms 15000 `
  --shortline-pct 0 `
  --rpc-rate-hz 2 --move-rate-hz 5 `
  --space-count 2 --cellapp-count 2 `
  --login-rate-limit-trusted-cidr 127.0.0.0/8 `
  --login-rate-limit-global 10000 `
  --with-cellappmgr-reviver `
  --cellappmgr-reviver-count 2 `
  --keep-cluster
```

另一个终端执行：

```powershell
tools\bin\verify_cellappmgr_ha.bat --min-cellapps 2 --min-revivers 2 --timeout-sec 90
```

Linux / macOS：

```bash
tools/bin/verify_cellappmgr_ha.sh --min-cellapps 2 --min-revivers 2 --timeout-sec 90
```

Reviver leader failover 基线：

```powershell
tools\bin\verify_cellappmgr_ha.bat `
  --min-cellapps 2 `
  --min-revivers 2 `
  --verify-reviver-failover `
  --max-reviver-failover-ms 10000 `
  --no-inject `
  --summary-json .tmp\cellappmgr_reviver_failover.json `
  --timeout-sec 90
```

要验证 leader 故障后仍保留一个 standby，可用 3 个 Reviver 启动集群，并执行：

```powershell
tools\bin\verify_cellappmgr_ha.bat `
  --min-cellapps 2 `
  --min-revivers 3 `
  --verify-reviver-failover `
  --min-post-failover-standbys 1 `
  --max-reviver-failover-ms 10000 `
  --no-inject `
  --summary-json .tmp\cellappmgr_reviver_redundancy.json `
  --timeout-sec 90
```

要验证连续两轮 leader 故障且每轮后仍保留一个 standby，可用 4 个 Reviver
启动集群，并执行：

```powershell
tools\bin\verify_cellappmgr_ha.bat `
  --min-cellapps 2 `
  --min-revivers 4 `
  --verify-reviver-failover `
  --reviver-failover-cycles 2 `
  --min-post-failover-standbys 1 `
  --max-reviver-failover-ms 10000 `
  --no-inject `
  --summary-json .tmp\cellappmgr_reviver_cascade.json `
  --timeout-sec 90
```

非破坏性巡检可加 `--no-inject`，脚本只验证当前 Reviver、snapshot 和
CellAppMgr watcher，并执行同一稳定窗口，不会关闭 CellAppMgr。若同时显式传
`--verify-reviver-failover`，脚本会关闭当前 active Reviver leader，但仍不关闭
CellAppMgr；它会要求 standby 获取 leader lock，direct heartbeat 重新变新鲜，
并确认 CellAppMgr pid 与 standby `launch_count` 不变。加
`--max-reviver-failover-ms N` 可把 active Reviver 退出到 standby heartbeat /
health 收敛耗时纳入 SLO 闸门；`--summary-json PATH` 会写出
`reviver_failovers` 明细和 `reviver_failover_*` latency 汇总；已启用的 SLO /
拓扑 gate 会在 `gates` 中记录指标名、观察值、最小值或最大值以及通过状态，
gate 未通过时也会先落盘；前置 watcher、拓扑或收尾检查失败时会写入
`current.run_healthy=false`、`failure_stage` / `failure_detail` 和
`run_health` gate。summary 也会记录 `gate_count`、`gate_failures`、
`run_health_checks`、`run_healthy` 和 `run_unhealthy`。`run_failure_stage`、
`current_failure_stages`、`cycle_failure_stages`、
`reviver_failover_failure_stages` 和 `failure_stages` 可直接按失败阶段归档；
`first_failure_stage` 和 `first_failed_gate` 暴露首因，脚本失败输出也会带
这两个首因字段；`failed_gate_names` / `failed_gates`、`overall_healthy` /
`overall_success_rate` 标识失败 gate 明细和整次验证是否通过所有 gate，供 CI
直接归档趋势。
若 Reviver leader failover 未收敛，也会先写入不健康的 `reviver_failovers[]`
明细，并以 `reviver_failover_health` gate 失败落盘。
`--max-load-report-age-ms N` 可把 CellApp reattach 后的 load 样本年龄纳入
同一 SLO 闸门；超龄会写入 `gates` 后失败，默认 0 表示只要求不 stale。
`--min-post-failover-standbys N` 会要求 leader 故障后仍保留 N 个 standby，
并在 JSON summary 中记录 `min_surviving_revivers` 和
`min_post_failover_standbys`。`--reviver-failover-cycles N` 会连续关闭当前
active Reviver leader，JSON 的每条 `reviver_failovers` 明细都包含 `cycle`。
脚本也会校验每个 standby Reviver 的 watcher 健康：必须不是 active leader、
`reviver/cellappmgr/status=standby`、`launch_pending=false`，且
`restart_limit_reached=false`。`--summary-json PATH` 的 `current.reviver_topology`
会在检查完成后重新读取最终 leader、registered / active / standby 数量和
`standby_health_ok`，`current.reviver_topology.standby_health` 会逐个列出 standby
Reviver 的 pid、leader-active、launch pending、restart limit 和健康结果，方便
CI 直接读取；最终或 `--no-inject` 巡检中的 standby watcher 不健康会以
`reviver_standby_health` gate 失败落盘。`current.recovery` 会结构化记录
reattach、restore gate、reattach registry 和 load report 的最终基础健康状态；
`current.stability_healthy` 记录最终稳定窗口是否通过，不健康会以
`stability_health` gate 失败落盘。
最终或 `--no-inject` 巡检中的 recovery 不健康会以 `recovery_health` gate
失败落盘。
`current.load_report` 会逐个
记录 CellApp 的 load age、stale 状态和 over-age 状态，便于 CI 直接做恢复后
load freshness 趋势归档。若启用 `--max-load-report-age-ms`，summary gate
会同时检查每轮接管和最终 `current.load_report` 的最大 load age。注入模式下，
每个 `cycles[]` 明细也包含同样的
`recovery` / `load_report` 字段，以及 per-cycle `healthy` 和 `failure_stages`，
便于定位具体故障轮次；若任一轮 takeover、stability、recovery 或 load report
不健康，会分别以
`cycle_takeover_health`、`cycle_stability_health`、`cycle_recovery_health` 或
`cycle_load_report_health` gate 失败落盘；summary 会聚合 successful / failed
cycle 数、takeover、stability、recovery / load report 健康检查数、健康 /
不健康数、load report 记录数、最大 load age 以及 stale / over-age app 总数。
连续接管基线可加 `--cycles N`；脚本会在每轮异常关闭前等待当前
CellAppMgr 写出 snapshot，并逐轮验证 Reviver retarget、reattach、独立日志和
稳定窗口。加 `--summary-json PATH` 可把每轮 PID / generation / launch delta、
snapshot、reattach 和稳定窗口结果落成机器可读摘要，并在 `parameters` 中记录
本次 `--min-*`、`--max-*` 和故障注入开关；`gates` 会记录已启用 SLO / 拓扑
gate 的观察值和通过状态；`--no-inject` 模式也会写出当前 watcher 巡检摘要。
加 `--max-takeover-ms N` 可要求每轮从 shutdown
到 revived CellAppMgr 写出 fresh snapshot 的耗时不超过阈值。生产 Reviver HA
拓扑可加 `--min-revivers 2`，脚本会要求只有一个 active leader，其余 Reviver
保持 standby。

验收：脚本打印 `PASS`，`old_pid != new_pid`，Reviver `active_generation`
递增，`heartbeat_acks > 0`，`cellappmgr/ha/snapshot_restores > 0`，并且
`reattach_pending=0`、`reattach_completed=1`、`reattach_stuck=0`、
`cellappmgr/lb/load_report_stale_count=0`，且若设置了
`--max-load-report-age-ms`，每个 CellApp 的 `load_age_ms` 都必须在阈值内；
同时 `reviver/cellappmgr/output_path` 指向的本地日志包含新 CellAppMgr pid。脚本
默认还会等待 5 秒稳定窗口，确认 Reviver 没有再次 retarget、restart、heartbeat
timeout 或 forced termination；可用 `--stability-sec` 调整。多轮模式下每轮
都必须满足同一验收。远程集群或旧配置
可用 `--allow-empty-output-log` 跳过本地日志文件校验。

交接验证时优先保留 `--summary-json` 输出。接手方先确认 `summary.overall_healthy`
为 true，`failed_gates` 为空，`successful_cycles` 等于 `cycles`，每条
`cycles[]` 的 `healthy` 为 true 且 `failure_stages` 为空；如果验证了 Reviver
leader failover，还要确认 `reviver_failovers[]` 全部 healthy，且
`current.reviver_topology.standby_health_ok=true`。脚本级 schema 回归按
Phase 13 验证基线中的三条命令执行，再进入 live fault injection。

`run_world_stress --with-cellappmgr-reviver` 未显式传 snapshot / lock 路径时，
会在 `.tmp/world-stress/<timestamp>/ha/` 下创建 CellAppMgr snapshot 和 Reviver
leader lock。`--cellappmgr-reviver-count N` 会启动 `reviver`、`reviver_01` ...
并共享同一 snapshot / leader lock；每个 Reviver 使用独立 internal port 和日志。
Reviver 拉起的新 CellAppMgr 输出会写到
`logs/cellappmgr_revived.log`，避免混入 Reviver 自身日志。driver 正常退出时会
先通过 machined 关闭 revived CellAppMgr，再停止其余已启动进程，避免 Reviver
拉起的 mgr 残留。

### 4.7 BaseAppMgr HA 验证

要让 Reviver 同时监督 BaseAppMgr,需要给 Reviver 启动配上
`--revive-baseappmgr-on-start true` 以及 BaseAppMgr exe/name/port:

```powershell
tools\bin\run_world_stress.bat `
  --clients 8 --account-pool 8 --duration-sec 20 `
  --ramp-per-sec 4 --hold-min-ms 15000 --hold-max-ms 15000 `
  --rpc-rate-hz 2 --move-rate-hz 5 `
  --space-count 2 --cellapp-count 2 `
  --login-rate-limit-trusted-cidr 127.0.0.0/8 `
  --login-rate-limit-global 10000 `
  --with-cellappmgr-reviver `
  --keep-cluster
```

(`run_world_stress` 当前 wrapper 把 BaseAppMgr 当作 driver-managed 进程,不通过
Reviver 启动。要让 Reviver 持 BaseAppMgr,目前需要手动启动一个独立 Reviver 进程
传入 `--revive-baseappmgr-*` 参数,或在 config JSON 的 `reviver.baseappmgr` 段
配置后让 `--with-cellappmgr-reviver` 的 Reviver 同时拿到 BaseAppMgr 目标。)

集群起来后:

```powershell
tools\bin\verify_baseappmgr_ha.bat --min-baseapps 1 --timeout-sec 90
```

Linux / macOS:

```bash
tools/bin/verify_baseappmgr_ha.sh --min-baseapps 1 --timeout-sec 90
```

非破坏性巡检:

```powershell
tools\bin\verify_baseappmgr_ha.bat --no-inject --summary-json .tmp\baseappmgr_ha.json
```

注入异常关闭并设 SLO gate:

```powershell
tools\bin\verify_baseappmgr_ha.bat `
  --cycles 2 `
  --max-takeover-ms 20000 `
  --summary-json .tmp\baseappmgr_ha_cycles.json
```

验收:脚本打印 `PASS`,`baseappmgr/ha/snapshot_saves` 在重启后增长,
`baseappmgr/ha/reattach_state=complete`,`reviver/baseappmgr/active_pid` 等于
新 BaseAppMgr pid,`reviver/baseappmgr/heartbeat_acks > 0`,且无 `last_error`。

跨机/多 Reviver lease 模式验证(需 `--revive-leader-lock-mode machined`
启动至少 2 个 Reviver):

```powershell
tools\bin\verify_baseappmgr_ha.bat `
  --min-revivers 2 `
  --check-leader-lock-mode machined `
  --verify-reviver-failover `
  --max-reviver-failover-ms 10000 `
  --no-inject `
  --summary-json .tmp\baseappmgr_lease_failover.json
```

`--check-leader-lock-mode machined` 会验证 `reviver/baseappmgr/leader/mode=machined`
且当前确实有一个 leader 持锁。`--verify-reviver-failover` 关掉当前 leader
Reviver,等待 standby 通过 lease 接管同一 BaseAppMgr(不重启 mgr)。
`--max-reviver-failover-ms` 把 shutdown→standby-active 的耗时纳入 SLO gate。
summary JSON 的 `current.leader_lock` 和 `current.reviver_failover` 段记录所有
观测到的指标,适合直接归档到 CI baseline。

### 4.8 P4 高密度 AoI

```powershell
tools\bin\run_world_stress.bat `
  --clients 50 `
  --account-pool 50 `
  --duration-sec 20 `
  --ramp-per-sec 25 `
  --hold-min-ms 10000 --hold-max-ms 10000 `
  --shortline-pct 0 `
  --rpc-rate-hz 2 --move-rate-hz 10 `
  --space-count 1 --cellapp-count 1 `
  --login-rate-limit-trusted-cidr 127.0.0.0/8 --login-rate-limit-global 10000
```

验证点：
- `aoi_enter` ≫ 0（单 space 下 N² fan-out 每轮产生 ~N × peers 个 enter 事件）
- `echo_rtt_p95` < 150 ms（单 CellApp 在 50-client 密度下可承受）
- 没有 `unexpected_disc` / 服务端 warning

已知缩放边界：**1 CellApp × 1 space 支持到大约 50 实体 × 10 Hz move + 2 Hz echo**。超过 100 实体/space 时 CellApp tick 被 AoI 广播挤爆，第二轮登录会卡在 `inflight`。突破这个边界需要 Space 拆分 / Cell offload。

### 4.9 真实客户端链路损伤

`--script-clients` 会启动真实 `atlas_client.exe` 子进程；可用
`--client-transport-impairment-ms` 给这些子进程的 BaseApp RUDP channel
注入双向延迟和 datagram loss。参数是每向延迟毫秒与万分比丢包率；例如 75ms
每向延迟 + 200/10000 丢包约等于 150ms RTT / 2% loss。
默认脚本程序集是 `bin/<build>/Atlas.ClientSample.dll`，runtimeconfig 会从
sample 项目输出自动转发。

```powershell
tools\bin\run_world_stress.bat `
  --clients 0 `
  --script-clients 2 `
  --script-verify `
  --duration-sec 20 `
  --client-transport-impairment-ms 75 200
```

`--client-drop-transport-ms` 仍用于窗口式全丢包恢复测试；持续损伤和 Phase14
移动预测验收使用 `--client-transport-impairment-ms`。

带 `--script-clients` 的运行结束后，`run_world_stress` 会打印
`Movement watcher summary`，把 BaseApp 输入 / ack / correction watcher 和
CellApp 输入队列 / step watcher 摘要到同一段输出，便于区分客户端 tap、
BaseApp relay 和 CellApp 权威 step 的断点。启用 `--script-verify` 时，
每个脚本客户端还必须看到 `mRpt` correction-report 发送日志；这些 watcher
也会成为闸门：BaseApp `in` / `fwd` / `ack` / `rpt` 与 CellApp `in` /
`frames` / `sim` / `hist` / `ack` 汇总都必须大于 0。BaseApp `rate` / `invalid` /
`seqgap` / `ackstale` / `rptdrop` 和 CellApp `rate` / `invalid` /
`seqgap` / `overflow` 必须为 0。

### 4.10 Phase14 虚拟客户端移动输入

裸协议客户端默认保留旧 `StressAvatar.ReportPos` 压测路径。要把 50/100/400
moving entities 压到 Phase14 服务端权威移动链路，使用 `--move-mode input`：

```powershell
tools\bin\run_world_stress.bat `
  --clients 50 `
  --account-pool 50 `
  --duration-sec 30 `
  --shortline-pct 0 `
  --move-rate-hz 10 `
  --move-mode input `
  --spread-radius 400 `
  --movement-verify `
  --login-rate-limit-trusted-cidr 127.0.0.0/8 `
  --login-rate-limit-global 10000
```

`--movement-verify` 会查询同一组 BaseApp / CellApp movement watcher，但不要求
`rpt > 0`，因为裸协议客户端只发送 input 并接收 ack，不运行 predictor replay
和 correction report。`move_sent` 是输入包发送数，`movement_ack_recv` 是裸协议
客户端收到的 `MovementStateAck` 数。

`--move-mode input` 下，`--spread-radius > 0` 会把裸协议客户端的初始象限编进
`SelectAvatar`，Stress Base 脚本用带 pose 的 `CreateBaseEntity` 创建
has_cell avatar，使首个 native movement state 落到对应 BSP leaf；随后仍会在
CellReady 后发送一次 stress-only `ReportPos`，但 `move_sent` 只统计 Phase14
input。400 档建议配合多 Space，例如
`--clients 400 --space-count 8 --cellapp-count 4 --spread-radius 400`。
`--movement-input-redundant-frames 2|3` 会把最近输入帧作为冗余帧打进同一包，
用于覆盖协议的 1..3 frame burst 和 CellApp stale 去重路径；此时 CellApp
`stale` watcher 允许大于 0。BaseApp `rate` / `invalid` / `seqgap` 必须为
0；CellApp `rate` / `invalid` / `seqgap` / `overflow` 必须为 0。
`--movement-input-drop-pct` 会在本地生成 seq 后丢弃整个 input 包，但仍保留
输入历史；配合 2/3 帧冗余可验证丢包恢复。`--movement-input-reorder-pct`
会把一个 input 包延迟到下一包之后再发送，用于触发 stale 去重。开启 drop /
reorder 时 BaseApp / CellApp `stale` 允许大于 0。BaseApp `rate` /
`invalid` / `seqgap` 必须为 0；CellApp `rate` / `invalid` / `seqgap` /
`overflow` 必须为 0。

当前 400 档基线：`spawn_pos_sent=400`、`move_sent=106221`、
`movement_ack_recv=14242`、`echo_loss=40`；4 个 CellApp 都有 movement input /
frame / ack 覆盖。BaseApp `rate` / `invalid` / `seqgap` / `ackstale` /
`rptdrop` 和 CellApp `rate` / `invalid` / `seqgap` / `overflow` 均为 0。
3 帧冗余短 smoke 基线：`move_sent=118`、`move_frames_sent=348`、CellApp
`stale=230`，BaseApp `rate` / `invalid` / `seqgap` 和 CellApp `rate` /
`invalid` / `seqgap` / `overflow` 仍为 0。
50 客户端 10% drop + 10% reorder 短 smoke 基线：`move_sent=4487`、
`move_client_drop=408`、`move_reordered=424`、`movement_ack_recv=1320`；
BaseApp `rate` / `invalid` / `seqgap` 和 CellApp `rate` / `invalid` /
`seqgap` / `overflow` 仍为 0。

### 4.11 P5 短线重登

```powershell
tools\bin\run_world_stress.bat `
  --clients 50 `
  --account-pool 50 `
  --duration-sec 60 `
  --ramp-per-sec 25 `
  --hold-min-ms 5000 --hold-max-ms 8000 `
  --shortline-pct 60 `
  --shortline-min-ms 1500 --shortline-max-ms 3000 `
  --rpc-rate-hz 2 --move-rate-hz 10 `
  --space-count 1 --cellapp-count 1 `
  --login-rate-limit-trusted-cidr 127.0.0.0/8 --login-rate-limit-global 10000
```

验证点：
- `unexpected_disc` = 0（客户端断开都是 `planned_disconnect`）
- `entity_transferred` 接近 `cell_ready`（过载时有差距，见 §6）

## 5. 关键参数

| 参数 | 作用 | 默认 |
|---|---|---|
| `--clients` | 并发虚拟客户端数 | 0 |
| `--account-pool` | 账号池大小；小于 clients 触发重登冲突 | 0 |
| `--duration-sec` | 总时长 | 30 |
| `--ramp-per-sec` | 每秒新 login 数 | 100 |
| `--hold-min/max-ms` | 普通会话在线时长 | 30s/60s |
| `--shortline-pct` | 触发短线断开的会话比例 | 0 |
| `--shortline-min/max-ms` | 短线断开触发窗口 | 1s/5s |
| `--rpc-rate-hz` | 进世界后每会话的 Echo 频率 | 2 |
| `--move-rate-hz` | 进世界后每会话的移动流频率 | 10 |
| `--move-mode` | `report-pos` 走旧 RPC；`input` 走 Phase14 movement input | `report-pos` |
| `--movement-input-redundant-frames` | 每个 movement input 包携带的输入帧数 | 1 |
| `--movement-input-drop-pct` | 本地生成后丢弃的 movement input 包比例 | 0 |
| `--movement-input-reorder-pct` | 延迟到下一包之后发送的 movement input 包比例 | 0 |
| `--movement-verify` | 用 movement watcher 验证裸协议 input 链路 | false |
| `--space-count` | Cell space 数量（按 session id 取模分配） | 1 |
| `--cellapp-count` | CellApp 进程数 | 1 |
| `--with-cellappmgr-reviver` | 启动 Reviver 监督 CellAppMgr | false |
| `--cellappmgr-reviver-count` | Reviver 进程数；>1 时共享 leader lock 形成 standby | 1 |
| `--reviver-port` | Reviver internal port | 27001 |
| `--reviver-port-stride` | 多 Reviver internal port 步长 | 1 |
| `--cellappmgr-snapshot-path` | CellAppMgr HA snapshot 文件；为空时随 Reviver 默认落到 `.tmp` | — |
| `--cellappmgr-snapshot-interval-ms` | CellAppMgr HA snapshot 周期 | 250 |
| `--reviver-leader-lock-path` | Reviver leader lock 文件；为空时随 Reviver 默认落到 `.tmp` | — |
| `--reviver-restart-delay-ms` | Reviver 异常重启延迟 | 1000 |
| `--reviver-heartbeat-timeout-ms` | Reviver direct heartbeat 响应超时 | 4000 |
| `--reviver-max-restarts` | Reviver 连续异常重启预算 | 3 |
| `--login-rate-limit-trusted-cidr` | 免 LoginApp per-IP 限流的 CIDR | — |
| `--client-transport-impairment-ms` | 真实脚本客户端 RUDP 每向延迟 + 万分比丢包 | — |

## 6. 已知限制和边界 ⚠️

### 6.1 单 CellApp × 单 space 的实体密度上限

约 50 活跃 StressAvatar @ 50m AoI 半径 @ 10 Hz move。超过此值 CellApp tick loop 被 witness 广播（O(N²)）饱和，auth/cell_ready 延迟飙升，第二轮登录可能卡 inflight。生产方案：space 切分 + cell offload。

### 6.2 短线 cell 实体 destroy 仅覆盖 "CellReady 已收到" 的会话

`BaseApp::FinalizeForceLogoff` 里的 `DestroyCellEntity` 只在 `BaseEntity::HasCell()==true` 时触发。那些在 `CellEntityCreated` ack 回到 BaseApp 之前就短线断开的会话：

- BaseApp 侧 Proxy 没绑 cell_addr → `HasCell()==false`
- CellApp 侧实际上已创建 cell 实体
- Destroy 路径跳过，cell 实体在 CellApp 上永久泄漏

实测（50 clients × 60% shortline × 40s）:
- `CreateCellEntity: 272`
- `DestroyCellEntity: 154` (57%)
- 差值 118 = 测试结束时仍在世界中的（健康）+ 上述竞态泄漏

**Follow-up**：BaseApp 需跟踪 "pending cell creates"（还没收到 ack 的实体），会话断开时也对这部分发 DestroyCellEntity。

### 6.3 客户端外部 RUDP 断开靠 inactivity timeout 检测

world_stress 断开只做 `network_.reset()`，不发 RUDP FIN。BaseApp 靠 10 秒 inactivity timeout 发现断开（见 `BaseApp::Init` 的 `SetAcceptCallback`）。实测效果：

- 客户端"主动"断开（shortline/planned）的检测延迟 ≈ 10s
- 影响实体生命期归还节奏，但不影响正确性

## 7. 运行产物

每次运行会在 `.tmp/world-stress/<timestamp>/` 生成：

- `logs/` — 所有进程的 stdout / stderr
- `db/atlas_world_stress.sqlite3` — DBApp 的 SQLite 库
- `dbapp.json` — 本次生成的 DBApp 运行配置
- `ha/` — CellAppMgr HA snapshot 和 Reviver leader lock（仅 Reviver 模式）
- `logs/cellappmgr_revived.log` — Reviver 拉起的新 CellAppMgr stdout/stderr

典型文件：

```
.tmp/world-stress/20260421-002500/
├── logs/
│   ├── baseapp.stdout.log
│   ├── cellapp.stdout.log
│   ├── cellapp_01.stdout.log   # 多 CellApp 模式
│   ├── dbapp.stdout.log
│   ├── loginapp.stdout.log
│   ├── baseappmgr.stdout.log
│   ├── cellappmgr.stdout.log
│   └── machined.stdout.log
└── db/
    └── atlas_world_stress.sqlite3
```

## 8. 结果判读

### 8.1 实时每秒汇总

```
[   5s] started=200 login_ok=200 auth_ok=200 login_fail=0 auth_fail=0
       timeouts=0 online=200 inflight=0 planned_disc=0 unexpected_disc=0
```

- `inflight` = 正在 login/auth 中途，应短暂非零
- `planned_disc` = shortline/hold 到期的主动断开（正常）
- `unexpected_disc` = 意外断开（**应始终为 0**）

### 8.2 最终 Summary（world_stress 独有字段）

| 字段 | 含义 | 健康值 |
|---|---|---|
| `entity_transferred` | 收到 EntityTransferred 通知（Account → Avatar 切换） | ≈ auth_success |
| `cell_ready` | 收到 CellReady 通知（cell 已绑定可发 cell RPC） | ≈ entity_transferred |
| `select_avatar_sent / fail` | SelectAvatar 本地发送结果 | fail = 0 |
| `echo_sent / received / rtt_{p50,p95,p99}` | 周期 Echo 回环与 RTT | p95 < 10 ms 常规 |
| `move_sent / fail` | 周期 ReportPos 本地发送 | fail = 0 |
| `aoi_enter / leave / pos_update / prop_update` | 收到的 AoI 信封分类计数 | 单 space 多 client 时 `aoi_enter` ≫ 0 |

### 8.3 判读快捷口径

1. `unexpected_disc > 0` → 服务端异常断开或协议违规；先看 baseapp.stdout.log 最后一段
2. `login_fail / auth_fail > 0` → LoginApp / DBApp 路径；看 loginapp.stdout.log + dbapp.stdout.log
3. `timeout_fail > 0` → 握手阶段超时；大多是 LoginApp 限速或 baseapp 拒绝
4. `cell_ready` << `entity_transferred` → cell 绑定滞后（高负载 / 短线过快）；看 §6.2
5. `echo_rtt_p95` 突增 → CellApp 处理延迟，常伴随 aoi/move 密度飙升

### 8.4 grep 出现频率 > 1 的 baseapp 日志模式

以下在压测完成后出现是**正常**的（不是失败）：

- `BaseAppMgr: BaseApp died` / `DBApp: BaseApp died` — 关停时 teardown 顺序产生
- `Slow tick: XX ms` — 只在启动瞬间出现一次

以下是真实问题信号：

- `client tried to call non-exposed X method (rpc_id=...)` — 客户端/服务端协议不匹配
- `ClientCellRpc dropped — no cell channel for target entity N` — cell 未就绪的 RPC（CellReady 之前发了 RPC），每秒限一次日志；看是不是客户端跳过了 CellReady 等待
- `RestoreEntity failed` / `restore_entity failed` — C# 侧实体材化挂了
- `cross-entity ClientCellRpc blocked` — 客户端拿着旧 entity_id 发 RPC；看 `TeardownNetwork` 是否正确重置 `entity_id_` 和 `echo_pending_`

## 9. 推荐流程

每次登录/cell/AoI 相关改动后，按序回归：

1. 单元+集成测试：`ctest --build-config Debug`
2. P2 最小活体（1 客户端）
3. P3 常规规模（200 客户端）
4. LB retire-drain 验证
5. CellApp crash/rehome 验证
6. CellAppMgr HA 验证
7. P4 AoI 密度（50 客户端单 space）
8. P5 短线重登
9. 检查 `logs/` 下 non-boilerplate 的 WARNING/ERROR

这样可以从小到大定位回归点。
