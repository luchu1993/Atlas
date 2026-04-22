# Script-Client Smoke 手册

运行 `world_stress --script-clients N` 的各种场景。`--script-clients` 模式会拉起真实的
`atlas_client.exe` 子进程（不是裸协议虚拟 client），通过子进程 stdout 解析
`[StressAvatar:<id>] <Event>` 日志计事件数，用来验证服务端属性 / 位置变更是否端到端到达客户端脚本
回调。

> 架构背景见 [PROPERTY_SYNC_DESIGN.md §5.1 / §5.1a / §5.1b](PROPERTY_SYNC_DESIGN.md)（baseline / backup 路径 + `OnXxxChanged` 触发规则）。本文档只讲"怎么跑、看啥数字"。

---

## 先决条件

- 集群跑在 `127.0.0.1`：`machined` + `LoginApp` + `BaseApp` + `CellApp` + `DBApp`
- `atlas_client.exe` 已构建（`build/debug/src/client/Debug/atlas_client.exe`）
- `Atlas.ClientSample.dll` 已构建（`samples/client/bin/.../Atlas.ClientSample.dll`）
- `entity_defs/StressAvatar.def` 的 `hp` 属性为 `reliable="true"`（默认值）

服务端节律：`samples/stress/Atlas.StressTest.Cell/StressAvatar.cs::OnTick` 以 1 Hz 把 HP 从 100 递减到 1 再回到 100 循环 —— 所有场景里 `hp` 的期望计数都以这个节律为基准。

---

## 场景总览（基线数字，2026-04-22）

4 个场景的典型输出。偏差 ≤±2 属正常 tick 边界抖动，偏差更大再复查。

| 场景 | 主题 | 命令（简化） | hp | seqgaps |
|---|---|---|---:|---:|
| 1 | AoI 广播事件计数（多 peer 负载） | `... --clients 50 --duration-sec 30` | ~669 | **0** |
| 2 | 应用层丢包（reliable 属性）| `... --client-drop-inbound-ms 5000 4000` | 31 | 8 |
| 3 | 传输层丢包（RUDP 重传验证） | `... --client-drop-transport-ms 5000 4000` | **39** | **0** |
| 4 | 应用层丢包（unreliable 属性 + baseline 兜底）| `python tools/cluster_control/run_unreliable_recovery.py` | 31-33 | 6-8 |

**关键对比**：

- **场景 2 vs 3** — 同样是 4 秒丢包窗口，**app 层 drop 丢 8 条事件**（`hp=31 seqgaps=8`），**transport 层 drop 由 RUDP 自动重传补齐**（`hp=39 seqgaps=0`）。这对组合是 reliable retransmit 工作的决定性证据。
- **场景 2 vs 4** — app 层 drop 对 reliable / unreliable 无区分（都丢 8 条），差异在**字段层面**：场景 4 下 baseline pump 每 ~6 s 静默拉回 `_hp` 真值，场景 2 下 reliable channel 已经 ACK 过所以无重传需要。脚本事件计数几乎一样。

**不变量**（场景 2-4 通用）：`hp + seqgaps ≈ duration_sec × peer_count × 1 Hz`，双 script client 互为 peer 时 `peer_count=2`。这个和对应服务端实际产生的 HP delta 总数。

---

## 场景 1：AoI 广播事件计数（多 peer 负载）

50 个裸协议虚拟 client（负载）+ 2 个脚本 client（观察）跑 30 秒，覆盖 AoI 广播 / 带宽预算 / 优先级回退的综合路径。

`run_world_stress.py` 透传 `--login-rate-limit-per-ip` / `--login-rate-limit-trusted-cidr` 给 LoginApp —— 所有本地 client 共享 `127.0.0.1`，默认 5/60s 的 per-IP 限额撑不起 50 个 client 并发登录：

```bash
python tools/cluster_control/run_world_stress.py \
    --clients 50 --account-pool 50 \
    --duration-sec 30 \
    --script-clients 2 \
    --script-verify \
    --login-rate-limit-per-ip 200 \
    --login-rate-limit-trusted-cidr 127.0.0.0/8
```

退出时 exit=0，典型 summary：

```
Summary
  login_success: 50 / auth_success: 50 / cell_ready: 50
  move_sent: 14880 (10 Hz × 30 s × 50 client)
  aoi_prop_update: 46850 / aoi_enter: 2550 / aoi_leave: 0
  echo_rtt_p50/p95: 793 / 3284 ms  (本地单机 CPU 吃紧时 RTT 明显升高)

[script-clients] per-child event summary:
  username              init  enterworld  destroy   hp  posupd   seqgaps  unparsed
  script_user_50         53          51        0  937      84         0        16
  script_user_51         53          51        0  937      83         0        16
```

**script-client 列读数**（30 秒 + 50 bare + 2 script，同一 50 m AoI 半径内）：

| 列 | 期望 | 说明 |
|---|---|---|
| `init` | 52-54 | 1 Account + 52 StressAvatar peer（50 bare + 对端 script + 本端 script；self 也计 OnInit） |
| `enterworld` | 51-52 | 非 self 的 peer enter 才触发（self 的 StressAvatar 走 EntityTransferred → CreateEntity 路径，未经 kEntityEnter） |
| `hp` | 800-1000 | 52 avatars × 30 s × 1 Hz ≈ 1560 个全量事件；AoI 进出、优先级回退、带宽预算会裁剪一部分；稳态稀疏约 60% 落地 |
| `posupd` | 50-200 | 每个 bare 10 Hz × 30 s = 300；script 看到的只有 AoI 半径内的 peer |
| `seqgaps` | **0** | 无丢包时 reliable delta 的 event_seq 必须连续；非零 = reliable channel 或 replication 路径有回归 |
| `unparsed` | ≈ 16 | init 阶段的 ClR / 网络日志行，非脚本事件 |

`--script-verify` 要求每个子进程至少观察到 1 次 `OnInit`，否则 exit code = 1。

---

## 场景 2：应用层丢包（reliable 属性）

2 个 script client 互为 peer，在第 5-9 秒由 **client 的应用层**（`client_app.cc::MainLoop` 默认 handler）丢弃入站 state-channel 消息。此时 RUDP 已经 ACK 过 —— 发送方不知道包丢了，**不会触发重传**，丢的事件就永久丢失，`event_seq` 跳号被 client 检出累加到 `seqgaps`。

```bash
python tools/cluster_control/run_world_stress.py \
    --clients 0 --duration-sec 20 \
    --script-clients 2 \
    --script-verify \
    --client-drop-inbound-ms 5000 4000
```

预期 summary（20 秒运行、4 秒丢窗口）：

```
[script-clients] per-child event summary:
  script_user_1        3           1        0   31       1        8        17
  script_user_2        3           1        0   31       1        8        17
```

验证 `hp + seqgaps = 31 + 8 = 39 ≈ 40`（2 avatars × 20 s × 1 Hz），8 个被丢的 reliable delta 精确转化为 8 个 `event_seq` 跳号。

---

## 场景 3：传输层丢包（RUDP 重传验证）

和场景 2 同样的丢窗口，但改由 **RUDP 层**（`ReliableUdpChannel::OnDatagramReceived`）在 ACK 生成之前丢包。发送方等不到 ACK → RTO → 重传，**reliable 流量全部恢复**，脚本事件不丢。

```bash
python tools/cluster_control/run_world_stress.py \
    --clients 0 --duration-sec 20 \
    --script-clients 2 \
    --script-verify \
    --client-drop-transport-ms 5000 4000
```

预期 summary：

```
[script-clients] per-child event summary:
  script_user_1        3           1        0   39       1        0        17
  script_user_2        3           1        0   39       1        0        17
```

`hp=39 seqgaps=0` —— 对比场景 2 的 `hp=31 seqgaps=8`，这就是 RUDP reliable retransmit 工作的证据。

---

## 场景 4：应用层丢包（unreliable 属性 + baseline 兜底）

把 `StressAvatar.hp` 标成 `reliable="false"`，跑场景 2 同样的丢窗口。unreliable 通道无 RUDP 重传，靠 **CellApp-side baseline pump** 每 `kClientBaselineIntervalTicks = 120` tick（~6 s）发一次全量 owner snapshot 来把 `_hp` 字段拉回真值。

一键 runner 包装了 patch → rebuild → smoke → restore，`try/finally` 保证 def 退出时一定回到 `reliable="true"`：

```bash
python tools/cluster_control/run_unreliable_recovery.py              # 默认 20s / drop 5..9s
python tools/cluster_control/run_unreliable_recovery.py --duration-sec 30
```

跑出来的 `hp / seqgaps` 和场景 2 几乎相同（因为 `--client-drop-inbound-ms` 是应用层过滤，不区分 reliable / unreliable）。**差异在字段层面而非脚本层面**：baseline 静默把 `_hp` 拉回服务端真值，脚本看不到补调的事件（见 [`PROPERTY_SYNC_DESIGN.md §5.1b`](PROPERTY_SYNC_DESIGN.md) 的 `isInitialising / shouldUseCallback` 契约）。

想看 baseline 实际收敛可以给 `StressAvatar.cs` 加周期性字段打印（`Hp` 值每 30 tick 打一次），观察 drop 窗口结束后 6 s 内值从 drop-前值漂移到服务端当前值。

---

## 两种丢包注入点对照

| Flag | 注入层 | 时机 | 用途 |
|---|---|---|---|
| `--drop-inbound-ms <s> <d>` | 应用层（`client_app.cc::MainLoop` 默认 handler） | RUDP 已 ACK 之后 | 验证 app 层的 gap 检测、baseline 兜底路径（场景 2 / 4） |
| `--drop-transport-ms <s> <d>` | 传输层（`ReliableUdpChannel::OnDatagramReceived`） | ACK 生成之前 | 验证 RUDP reliable 重传路径（场景 3） |

两个 flag 可以同时开（应用层先丢、传输层再丢），实际场景通常只开一个。

---

## 日志格式契约

`samples/client/StressAvatar.cs` 的日志形态被 `src/tools/world_stress/client_event_tap.cc` 按字面量解析。

| 脚本日志 | Tap 匹配 | 触发通道 |
|---|---|---|
| `[StressAvatar:42] OnInit` | `on_init` | 工厂刚创建实例（enter 或玩家自身） |
| `[StressAvatar:42] OnEnterWorld pos=(..) hp=..` | `on_enter_world` | `kEntityEnter` 信封初始快照应用完毕 |
| `[StressAvatar:42] OnDestroy` | `on_destroy` | `kEntityLeave` 或登出 |
| `[StressAvatar:42] OnHpChanged old=X new=Y` | `on_hp_changed` | **只由 delta 通道触发**（`0xF001` / `0xF003` kEntityPropertyUpdate → `ApplyReplicatedDelta`）；**baseline `0xF002` 不触发**（`ApplyOwnerSnapshot` 直写字段） |
| `[StressAvatar:42] OnPositionUpdated pos=(..)` | `on_position_updated` | volatile delta `0xF001` kEntityPositionUpdate |
| `[StressAvatar:42] event_seq gap: last=A got=B missed=N` | `event_seq_gaps += N` | 客户端检测到 `kEntityPropertyUpdate` 的 u64 seq 前缀跳号 |

每条日志前还有 `[t=<seconds>.sss]` 前缀（`Atlas.Client.ClientLog`）—— tap 会 strip 掉不参与匹配，但后续收敛分析可以 `grep '^\[t=\([0-9.]*\)\]'` 把时间戳抽回来对 drop window 边界做自动断言。

修改日志格式时两边同步更新，否则 `unparsed_lines` 会飙升且对应计数会失准。

---

## 注意事项

- `atlas_client.exe` 的脚本事件走 `Console.WriteLine` → 子进程 stdout → world_stress 解析。压测规模 ≥ 几十 script-client 时管道会成为瓶颈，此时用 `--clients N`（裸协议）做负载、`--script-clients 2` 做观测点即可。
- `--drop-inbound-ms` / `--drop-transport-ms` 可同时开（应用层先丢、传输层再丢），但通常只开一个；想验证 reliable 重传用 transport 版本，想验证 gap 检测 / baseline 路径用 inbound 版本。
