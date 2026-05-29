# Phase 13: 高可用 — Reviver + Manager Recovery

**Status:** 🚧 CellAppMgr / BaseAppMgr 已切换到 BigWorld 式 worker-重建恢复
（M1 + M2 落地）：manager 是软状态，崩溃后由 Reviver 重启，新进程在 recovery
窗口内等存活 worker 重报状态并重建注册表 / partition——**不再有 manager
self-snapshot、`mgr_generation` epoch、snapshot 文件或 reattach 对账机制**。
Reviver 监督全局唯一 CellAppMgr 和 BaseAppMgr（multi-target），支持 local /
machined-lease leader lock、cold-start、direct heartbeat、manager health 和
registry audit。**待落地：** M3（Reviver priority+timeout 仲裁替换 machined-lease）、
M4（machined → per-host UDP 广播网格，不可逆点）。
**前置依赖:** Phase 11（分布式空间完整可用）
**BigWorld 参考:** `server/reviver/`, `server/dbappmgr/`

## 目标

为 Atlas 集群添加 BigWorld 风格的 Manager 高可用能力。全局唯一 Manager
由 Reviver 监督；Manager 崩溃后从存活 worker 重建权威状态，普通 App 的故障
由对应 Manager 处理。

## BigWorld 对齐（源码实证）

对照 BigWorld 源码（`programming/bigworld/`）确认其 HA 三原则：

- **Manager 是软状态，靠 worker 重建**：`server/cellappmgr/cellappmgr.cpp`
  `startRecovery()`（~L2258）只起一个 ~2s 计时器等 worker 上报；存活的
  CellApp/BaseApp 经 birth listener 发现新 manager 后主动调
  `recoverCellApp()` / `recoverBaseApp()` 重报状态，manager 现场 `addApp()`
  重建注册表。**manager 不向磁盘持久化自身协调态。** 真正的持久态在 DB（DBMgr）。
- **去中心 machined**：`server/tools/bwmachined/` 每台机一个守护进程，UDP
  广播发现（`lib/network/machine_guard.cpp` `BROADCAST`），按 IP 组 ring
  （`cluster.cpp`）；单例 manager 靠"广播查询 → 注册表 first-found"，**无
  集群级硬锁**。
- **软仲裁、无 fencing**：`lib/server/reviver_subject.cpp`（~L97）—— 多
  Reviver 各带 `ReviverPriority`（`reviver_common.hpp` uint8），被监控对象
  端按 priority + 心跳超时（`REVIVER_DEFAULT_SUBJECT_TIMEOUT` 0.2s）裁决谁
  活跃；无显式 fencing token，靠死地址 + 新端点 + 注册表收敛。

**已采纳目标：完全对齐 BigWorld 实现**（不保留偏离）。**已知代价**：UDP 广播
machined 在 k8s / 云 overlay 网络通常不可路由——完成 machined 去中心化（M4）后
Atlas 不再支持容器 / 云部署；纯 worker 重建在"全集群同时重启"时丢失 manager
态（与 BigWorld 同）。

### 迁移进度（每阶段独立可 build + 测试；M4 = 不可逆点）

- **M1 worker 重建** ✅（commit `63ca552` `efc76ff` `d101231` `3632e34`）：CellApp
  注册后经 `RecoverCellAppState` 上报它持有的整树（每 space 的 `bsp_blob` +
  geometry_version），mgr 取最高版本反序列化重建 partition；启动 recovery 窗口
  （`recovery_deadline_`，复用 startup 收敛窗口）内冻结 LB/grow/split/retire，
  等 worker 报告到齐；`RegisterCellApp` 携带 `known_app_id`，保留原 app_id
  （EntityID 高字节路由不破）。
- **M2 删 snapshot + epoch + 回放残留** ✅（commit `4885158` `a589e8b` 及 M2c）：
  - **M2a**：删 `mgr_generation` epoch——BigWorld 无 fencing，靠端点身份拒绝
    旧 mgr 消息（`AcceptCellAppMgrMessage` 按 channel identity，
    `cellappmgr_stale_drops` 计数）。
  - **M2b**：删整个 manager snapshot 子系统（`Snapshot`/`Restore`/snapshot
    文件 / 周期与 dirty flush / ~40 个 snapshot watcher / config / CLI）。
    snapshot 是 reattach 机制的唯一种子，所以 reattach watchdog / registry
    对账 / force-resolve 一并移除；restore gate 坍缩为 M1 recovery 窗口
    （`cellappmgr/ha/recovery_window_active` + `recovery_window_status`）。
  - **M2c**：删 mgr→worker 回放残留（`ReplayCellAppTopology` /
    `RequestCellAppState`）——worker 重注册后主动重报 BSP 并恢复周期 InformLoad，
    mgr 无需回放或轮询；补 BaseApp `known_app_id` echo（对称于 CellAppMgr）。
- **M3 Reviver priority+timeout 仲裁**（待落地）：被监控对象端按 priority +
  心跳超时裁决唯一活跃 Reviver，删 machined-lease leader lock。在 TCP machined
  上做。
- **M4 machined → per-host UDP 广播网格**（待落地，不可逆点）：每台机一个
  machined，UDP 广播发现 + ring/buddy，单例 manager 靠"广播查询 → first-found"，
  删单地址 TCP 中心模型。依赖 M3。

## 当前已落地能力

- **CellAppMgr worker 重建恢复**：`OnRecoverCellAppState` 收集存活 CellApp 上报的
  per-space `{geometry_version, bsp_blob}`，按 space 取最高版本反序列化重建
  `spaces_`，从 BSP leaf 的 `cellapp_addr` 重建 cell→app 归属（只在该 space 所有
  leaf owner 都已注册时才重建，路由才能解析）。`RegisterCellApp.known_app_id`
  让 snapshot-less revive 保留原 app_id；冲突或越界时回退到 id pool 分配。
- **recovery 窗口（topology 冻结）**：`recovery_deadline_` 在 Init 按
  `startup_quiescence_window_`（默认 ~2s）设置；窗口内 `RecoveryWindowActive()`
  为真，冻结 LB tick / elastic grow / auto split-merge / retire drain，等 worker
  把 BSP 报齐再对完整视图做均衡。watcher：`cellappmgr/ha/recovery_window_active`
  （bool）、`cellappmgr/ha/recovery_window_status`（`state` / `active` /
  `remaining_ms` / `pending_geometry`）。
- **BaseAppMgr worker 重建恢复**：BaseApp 表无 BSP，崩溃后由 BaseApp 重注册重建；
  `RegisterBaseApp.known_app_id` 让重注册保留原 app_id，InformLoad 路由不破。
- **stale-mgr 防护**：CellApp 侧 `AcceptCellAppMgrMessage` 按 channel 身份丢弃非
  当前 mgr 的残留 control-plane 消息（`cellapp/ha/cellappmgr_stale_drops`
  watcher）；无显式 generation/fencing（与 BigWorld 一致）。partition / recovery
  窗口内旧 mgr 的在途包不会污染拓扑决策。
- **CellApp reattach（轻量）**：mgr 重启后表为空，存活 CellApp 经 machined
  birth / death 重连并重注册（保留 app_id），随即主动重报 BSP。重复注册
  （同 channel）幂等 re-ack；同 pid / 地址的 birth replay 被忽略，新 pid 或
  新地址会触发重连。
- **Reviver 监督全局唯一 Manager**：Reviver 重构为 multi-target，持有
  `cellappmgr_target_` 和 `baseappmgr_target_`，各自独立 leader lock、heartbeat、
  launch 预算、watcher 路径（`reviver/cellappmgr/*`、`reviver/baseappmgr/*`，
  CellAppMgr 兼容 legacy `reviver/leader/*` alias）。订阅 machined birth / death
  按进程名匹配目标，启动先查 registry 避免与已存在 manager 重复 cold-start。
- **Reviver cold-start / restart**：`--revive-cellappmgr-on-start` /
  `--revive-baseappmgr-on-start` 可在 machined 中无目标时拉起新进程；abnormal
  death 通知后延迟重启，默认最多 3 次连续尝试，新 manager direct heartbeat ack
  后重置预算；`revive-restart-backoff-cap-ms` > 0 时按 attempts 指数退避并 cap
  截断。启动后未在 `revive-cellappmgr-launch-timeout-ms` 内注册按失败重试，预算
  耗尽后经 watcher 暴露报警状态。
- **Reviver direct heartbeat**：主 Reviver 通过 manager 二进制 `HealthProbe` /
  `HealthProbeAck`（nonce + game_time）验证目标 RUDP 控制面仍响应；heartbeat
  超时独立配置、连续失败独立计数，不被 watcher health 成功掩盖。
- **Reviver manager health / liveness / registry audit**：主 Reviver 通过 machined
  watcher forwarding 查 `app/uptime_seconds` 确认事件循环响应；对本地目标检查
  PID liveness；周期查 machined registry，活跃目标连续缺失即走异常重启路径。
  direct heartbeat 与 manager watcher 使用独立连续失败计数，任一达阈值都先终止
  旧 pid 再重启，避免挂死进程占用 internal port。
- **Reviver leader lock**：启动后获取目标的进程级 leader lock；同机多实例只有
  持锁者会 cold-start / restart，standby 保留进程和 watcher 但不动 Manager。
  支持两种模式（见下"当前边界"）。
- **CellApp failure smoke**：普通 CellApp 崩溃由 CellAppMgr rehome（unsplit /
  rehome 孤儿 leaf）和 BaseApp death-restore 路径处理；`verify_cellapp_rehome.{bat,sh}`
  校验拓扑 rehome、BaseApp restore scheduled/restored/lost/timeout/pending 收敛、
  payload / Ghost backup / promote 来源覆盖与占比，支持多轮注入和机器可读 summary。

## 关键设计决策

### Manager 从 worker 重建状态（无 snapshot）

CellAppMgr 是 BSP 权威，但它**不持久化**自身协调态：崩溃后 Reviver 重启它，
新进程起 recovery 窗口等存活 CellApp 重报各自持有的整树（`RecoverCellAppState`），
按 space 取最高 geometry_version 重建 partition；BaseAppMgr 同理由 BaseApp 重注册
重建表。这忠实对应 BigWorld 的 "worker 报告自身状态、manager 重建"，避免了跨机
snapshot 一致性难题。代价：全集群同时重启会丢失 manager 态（与 BigWorld 同）。

### Reviver 只监督全局唯一 Manager，不重建业务状态

Reviver 负责"让唯一的 Manager 活着"——cold-start / restart / 终止挂死进程；它
**不**尝试恢复 BSP 或 BaseApp 表，那是 Manager 自己从 worker 重建的职责。同机多
Reviver 通过 leader lock 选唯一主动监督者。

## 交接状态

CellAppMgr / BaseAppMgr HA 当前是同机 HA MVP 和 CI / 压测基线：Reviver 监督全局
唯一 Manager，Manager 崩溃后从存活 worker 重建权威状态；CellApp 存活时通过重连 +
重注册 + BSP 重报收敛，recovery 窗口期间冻结 topology 推进。

后续接手先跑脚本级 + 单测回归，再跑 live fault injection（见下"验证基线"）。
**注意：** `verify_cellappmgr_ha.py` / `verify_baseappmgr_ha.py` 仍按旧 snapshot
模型断言（snapshot restore / heartbeat snapshot 摘要 / reattach 收敛），M2 删除
snapshot 后这些 live 脚本需要改写为校验 worker-重建恢复——排在 M3（Reviver 仲裁
重构）之后做，避免 HA 模型未定型时返工。

## 当前边界

- Reviver 监督 CellAppMgr 和 BaseAppMgr（multi-target）；DBAppMgr 拆到
  Phase 15（`phase15_dbappmgr.md`）。
- Reviver leader lock 现支持两种模式：
  - `local`（默认）：per-host 文件锁，单机有效；
  - `machined`：lease 由 machined 持有，跨机 Reviver 可竞争同一 key。machined 在
    `OnTickComplete` 周期 prune 过期 lease，disconnect 时 drop 当前 channel 持有的
    全部 lease。`--revive-leader-lock-mode machined` 切换；
    `--revive-leader-lock-ttl-ms`（默认 8s）/ `--revive-leader-lock-renew-ms`
    （默认 3s）/ `--revive-leader-lock-failure-threshold` 控制 renew 节奏与放弃
    leadership 的阈值。
  跨机集群仍需要 machined 本身可用——machined HA 不在 Phase 13 范围。**M3 将用
  BigWorld 式 priority+timeout 仲裁替换 machined-lease。**
- **lease drop-on-disconnect 语义**（machined 模式）：Reviver ↔ machined 的 TCP
  控制连接断开时 `MachinedApp::OnDisconnect` 立即删除该 Reviver 的全部 lease，不等
  ttl。优点：Reviver 崩溃 / 掉电时 standby 在下一个 audit tick（默认 1s）即可
  acquire，active 切换 P95 保持秒级。代价：瞬时网络抖动也触发 lease 转移，ttl 设
  过小会看到 lease ownership 频繁切换。可观测：
  `machined/leases/dropped_on_disconnect_total` / `pruned_total` / `active`。
- **machined 不可达 ops 剧本**（machined 崩溃 / 网络分区）：Reviver 侧
  `reviver/{slug}/leader/lease_failure_count` 持续增长、`last_error` 含
  "machined not connected" / "lease request timed out"、
  `reviver/{slug}/leader/active` 在阈值次 renew 失败后变 false；被监督 Mgr 的
  `heartbeat_acks` 停止增长。Mgr 本身 RUDP 控制面对 worker 仍跑，但 machined
  listener 无法响应远程 watcher 查询。**窗口期内 Mgr 死亡不会自动重启，需 ops 介入；**
  machined 恢复后 Reviver 自动重新竞争 lease。建议把 `reviver/+/last_error` 接告警。
- 异常检测已有 machined death 通知、direct heartbeat、manager watcher health、本机
  PID liveness 和 registry audit；跨机器误判防护仍依赖 leader lock / 共享状态方案。
- 全集群同时重启会丢失 manager 态（与 BigWorld 同）；单 manager 重启由存活 worker
  重报恢复。
- restore 后始终不重连的 CellApp：M2 删除 reattach 对账后，mgr 表里本就不存在
  "待重连"的 ghost（表为空、靠 worker 主动重报），所以无需 force-resolve；machined
  registry 是 CellApp 是否存活的真相来源。
- DBAppMgr 多 DBApp、分片迁移和 DBApp 故障转移仍未实现（Phase 15）。
- BaseApp crash 后的客户端 session resume 尚未实现；当前仍走重新登录路径。
- Reviver multi-target 目前由 cellappmgr_process 集成测试 + baseappmgr in-process
  单测 + baseappmgr_messages round-trip 三层覆盖，无真实 process 级 multi-target
  端到端验证；F1 multi-target 重构后两 target 走同一 `ManagedTarget` 参数化路径，
  回归风险低，留待 Phase 15 三 target 一起补 process 级集成测试。

## BaseAppMgr HA

- **worker 重建**：BaseApp 表（internal/external addr、app_id、is_ready、
  is_retiring）+ `dbid_affinity_` 由 BaseApp 重注册重建；无 snapshot。
- **known_app_id echo**：`RegisterBaseApp.known_app_id` 让重注册保留原 app_id；
  echoed id 空闲则保留，否则从 pool 分配（镜像 CellAppMgr）。
- **重复注册**：同 internal addr 的重复注册被拒（duplicate），不消耗 app_id。
- **Reviver 扩展**：BaseAppMgr 加 `HealthProbe` / `HealthProbeAck`（msg id 6020/
  6021，nonce + game_time），Reviver 通过它校验 manager RUDP 控制面响应；watcher
  路径 `reviver/baseappmgr/*` 与 CellAppMgr 对齐。
- **ServerConfig**：`revive_baseappmgr_*`（exe、name、internal_port、output_path、
  update_hertz、launch_timeout_ms、on_start、leader_lock_path）平行于
  `revive_cellappmgr_*`，支持 CLI 和 `reviver.baseappmgr` JSON 段。目标 enabled 当
  on_start 或 exe/port 任一已配置——未配置时该 target 完全 silent，旧 single-target
  Reviver 行为保持不变。
- BaseAppMgr 死亡时 BaseApp 自身的 entity restore 走 Phase 13 原有 CellApp
  death-restore 路径（BaseApp 不受 BaseAppMgr 重启直接影响）。

## 后续工作

1. **M3 — Reviver priority+timeout 仲裁**：多 Reviver 各带 priority，被监控对象端
   按 priority + 心跳超时裁决唯一活跃 Reviver，删 machined-lease leader lock。
2. **M4 — machined per-host UDP 广播网格**（不可逆点）：每台机一个 machined，UDP
   广播发现 + ring/buddy，删单地址 TCP 中心模型；断容器 / 云部署。
3. **verify 脚本改写**：把 `verify_cellappmgr_ha.py` / `verify_baseappmgr_ha.py`
   从 snapshot-restore 断言改为 worker-重建恢复断言（recovery 窗口收敛、worker
   BSP 重报、known_app_id 保留、topology fingerprint 连续性）。排在 M3 之后。
4. **DBAppMgr 多 DBApp registry + HA** — 详见 `phase15_dbappmgr.md`，按 worker-重建
   原则实现（不再用 snapshot）。

## 验证基线

- `tests/unit/test_cellappmgr.cpp`：CellAppMgr 注册 / app_id 分配、`known_app_id`
  snapshot-less recovery 与冲突回退、recovery 窗口冻结 / 开启
  （`recovery_window_active` / `recovery_window_status`）、worker 重报重建 space
  （`RecoverCellAppState`，leaf owner 未知时跳过、version 不更新时忽略）、LB
  split / merge / retire drain / handoff、CellApp death rehome / unsplit、pending
  geometry 冻结与 ack。
- `tests/unit/test_cellapp_handlers.cpp`：CellApp 注册后主动重报、load report flush
  内容（script tick 贡献、失败重试保留计数）、CellAppMgr birth replay 判定（同
  pid/地址不重连，新 pid / 新地址 / 旧 channel down 后重连）。
- `tests/unit/test_cellappmgr_messages.cpp` / `test_baseappmgr_messages.cpp`：
  `HealthProbeAck`（nonce + game_time）round-trip 与截断拒绝、`RegisterBaseApp`
  `known_app_id` round-trip、msg id 范围。
- `tests/unit/test_server_config.cpp`：Reviver CLI 与 JSON 配置解析。
- `tests/integration/test_baseappmgr_registration.cpp`：重复注册拒绝且不消耗
  app_id、echoed `known_app_id` 保留 + 冲突回退、spoof 防护、分配 / dbid affinity /
  stale load 跳过。
- `tests/integration/test_cellappmgr_process.cpp`：真实 Reviver cold-start
  CellAppMgr、异常终止后重启到同一 internal port、同机多 Reviver leader lock 防止
  重复 cold-start、late Reviver attach 不重启已有 mgr、leader 退出后 standby 接管、
  direct heartbeat / forced termination / restart limit / manager health watcher
  timeout / launched 进程不注册重试，并验证 revived CellAppMgr 继承配置中的 LB 权重。
- `tests/integration/test_machined_registration.cpp`：machined 转发 shutdown 后目标
  deregister 的 death notification 保留原 reason。
- `tools/bin/verify_cellapp_rehome.{bat,sh}`：CellApp crash app-level HA smoke
  （死亡 rehome、BaseApp restore 来源拆分、恢复量 / 耗时 / 占比闸门、机器可读
  summary）。
- MVP smoke 已验证 CellAppMgr / CellApp LB 路径在 HA 改动后仍可登录、auth、绑定
  客户端并进入 4-leaf Space。
