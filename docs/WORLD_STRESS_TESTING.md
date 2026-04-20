# Atlas 全链路压测说明

> 更新时间: 2026-04-21
> 适用范围: LoginApp / BaseApp / DBApp / BaseAppMgr / CellAppMgr / CellApp 的端到端登录、世界态进入、cell RPC、AoI、短线重登压测

## 1. 目标

`world_stress` 是 `login_stress` 的超集，覆盖从客户端握手到 cell 侧 CLR 派发的整条链路：

```
client
   │
   ├── LoginRequest     → LoginApp           (认证)
   ├── Authenticate     → BaseApp            (账户实体材化)
   ├── ClientBaseRpc    → Account.SelectAvatar (脚本创建 StressAvatar)
   │                       └── CreateBaseEntity native API
   │                             ├── RestoreEntity (C# 实例化)
   │                             └── CreateCellEntity → CellApp
   │                                   └── OnCreateCellEntity + 自动 EnableWitness
   ├── ← EntityTransferred (客户端切换到 StressAvatar)
   ├── ← CellReady         (cell 已绑定，安全发 cell RPC)
   ├── ClientCellRpc → StressAvatar.Echo  (cell 侧 RPC 回环)
   │     └── Client.EchoReply → SelfRpcFromCell → client
   ├── ClientCellRpc → StressAvatar.ReportPos (位置更新 + AoI 广播)
   │     └── Position 属性 → 邻居收 kEntityPositionUpdate
   └── Disconnect / shortline / reconnect
         └── OnExternalClientDisconnect → FinalizeForceLogoff → DestroyCellEntity
```

## 2. 工具与脚本

### 2.1 集群拉起

- `tools/cluster_control/run_world_stress.py`

相对 `run_login_stress.py` 的增量：
- 启动 CellAppMgr + N 个 CellApp
- BaseApp / CellApp 的 `--assembly` 指向 `samples/stress/Atlas.StressTest.{Base,Cell}`
- 新参数：`--cellapp-count`、`--space-count`、`--rpc-rate-hz`、`--move-rate-hz`

### 2.2 压测客户端

- `src/tools/world_stress/main.cc` — 继承 login_stress 的 Session 状态机

## 3. 环境前提

- 构建目录：`build/debug`（默认，可用 `--build-dir` 覆盖）
- 以下程序必须存在：
  - `machined` / `atlas_loginapp` / `atlas_baseapp` / `atlas_baseappmgr` / `atlas_dbapp`
  - `atlas_cellapp` / `atlas_cellappmgr`
  - `atlas_tool`
  - `world_stress`
- C# Runtime：
  - `runtime/atlas_server.runtimeconfig.json`
  - `samples/stress/Atlas.StressTest.Base/bin/x64/Debug/net9.0/Atlas.StressTest.Base.dll`
  - `samples/stress/Atlas.StressTest.Cell/bin/x64/Debug/net9.0/Atlas.StressTest.Cell.dll`
- 端口段（默认）：
  - 20013 LoginApp external
  - 20018 machined
  - 21001+ BaseApp internal
  - 22001+ BaseApp external
  - 23001 BaseAppMgr
  - 24001 DBApp
  - 25001 CellAppMgr
  - 26001+ CellApp

## 4. 推荐执行方式

### 4.1 P1 集群启动自检（不跑客户端）

```powershell
python tools/cluster_control/run_world_stress.py `
  --clients 0 `
  --duration-sec 10
```

期望：7 个进程都注册到 machined，10 秒 hold 无错误，优雅退出。

### 4.2 P2 最小活体（端到端闭环）

```powershell
python tools/cluster_control/run_world_stress.py `
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
python tools/cluster_control/run_world_stress.py `
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

### 4.4 P4 高密度 AoI

```powershell
python tools/cluster_control/run_world_stress.py `
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

已知缩放边界：**1 CellApp × 1 space 支持到大约 50 实体 × 10 Hz move + 2 Hz echo**。超过 100 实体/space 时 CellApp tick 被 AoI 广播挤爆，第二轮登录会卡在 `inflight`。突破这个边界需要 Phase-11+ 的 Space 拆分 / Cell offload。

### 4.5 P5 短线重登

```powershell
python tools/cluster_control/run_world_stress.py `
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
| `--move-rate-hz` | 进世界后每会话的 ReportPos 频率 | 10 |
| `--space-count` | Cell space 数量（按 session id 取模分配） | 1 |
| `--cellapp-count` | CellApp 进程数 | 1 |
| `--login-rate-limit-trusted-cidr` | 免 LoginApp per-IP 限流的 CIDR | — |

## 6. 已知限制和边界 ⚠️

### 6.1 单 CellApp × 单 space 的实体密度上限

约 50 活跃 StressAvatar @ 50m AoI 半径 @ 10 Hz move。超过此值 CellApp tick loop 被 witness 广播（O(N²)）饱和，auth/cell_ready 延迟飙升，第二轮登录可能卡 inflight。生产方案：space 切分 + cell offload（Phase 11+）。

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
4. P4 AoI 密度（50 客户端单 space）
5. P5 短线重登
6. 检查 `logs/` 下 non-boilerplate 的 WARNING/ERROR

这样可以从小到大定位回归点。
