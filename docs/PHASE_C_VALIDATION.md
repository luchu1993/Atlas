# Phase C 验证 Runbook

> 架构背景见 [PROPERTY_SYNC_DESIGN.md §5.1 / §5.1a / §5.1b](PROPERTY_SYNC_DESIGN.md)（baseline / backup 路径 + `OnXxxChanged` 触发规则）。本文档只管"怎么跑、看啥数字"。

## 基线结果快照（2026-04-22 跑一遍完整 sweep）

4 个场景的典型输出。偏差 ≤±2 属正常 tick 边界抖动，偏差更大再复查。

| 场景 | 命令（简化）| init | enterworld | hp | posupd | seqgaps |
|---|---|---:|---:|---:|---:|---:|
| **C2** 50 bare + 2 script, 30s | `... --clients 50 --duration-sec 30` | 53 | 51 | ~669 | ~84 | **0** |
| **C3-A** 2 script, 20s, app drop 5..9s | `... --client-drop-inbound-ms 5000 4000` | 3 | 1 | 31 | 1 | 8 |
| **§4** 2 script, 20s, **transport** drop 5..9s | `... --client-drop-transport-ms 5000 4000` | 3 | 1 | **39** | 1 | **0** |
| **C3-B** reliable=false + app drop 5..9s, 20s | `python tools/cluster_control/run_unreliable_recovery.py` | 3 | 1 | 31-33 | 1 | 6-8 |

**场景间关键对比**：
- C3-A vs §4：同样是 4 秒丢包窗口，**app 层 drop 丢 8 条事件 (`hp=31 seqgaps=8`)，transport 层 drop 全部重传补齐 (`hp=39 seqgaps=0`)** —— 决定性证明 RUDP reliable retransmit 工作。
- C3-A vs C3-B：app 层 drop 对 reliable / unreliable 无区分（都丢 8 条），差异在字段层面：C3-B 下 L4 baseline pump 在 ≤6 s 后静默恢复 `_hp` 真值，C3-A 下 reliable channel 已经 ACK 所以无重传。

Phase C 把 Phase B 的客户端回调契约放到真实集群里跑一遍。本文档覆盖两条验证链路：

- **C2（事件可达）**：服务端属性/位置变更 → 客户端脚本钩子触发
- **C3（失联恢复）**：客户端在窗口内丢弃入站 state-channel 消息，窗口结束后状态最终收敛

两条链路都复用同一套工具 —— `world_stress` 的 `--script-clients` 模式启动真实的 `atlas_client.exe` 子进程，通过 stdout 解析 `[StressAvatar:<id>] <Event>` 日志行计数事件。所有必要的脚本钩子由 `samples/client/StressAvatar.cs` 提供，服务端 HP 节律由 `samples/stress/Atlas.StressTest.Cell/StressAvatar.cs::OnTick` 驱动（1 Hz，100 → 1 → 100 循环）。

## 先决条件

- 集群跑在 `127.0.0.1`：`machined` + `LoginApp` + `BaseApp` + `CellApp` + `DBApp`
- `atlas_client.exe` 已构建（`build/debug/src/client/Debug/atlas_client.exe`）
- `Atlas.ClientSample.dll` 已构建（`samples/client/bin/.../Atlas.ClientSample.dll`）
- `entity_defs/StressAvatar.def` 的 `hp` 属性为 `reliable="true"`（默认值）

## C2：事件可达链路

一次命令同时启动 50 个裸协议虚拟客户端（负载）+ 2 个真实脚本客户端（观察）。`run_world_stress.py` 前端的 `--login-rate-limit-per-ip` 和 `--login-rate-limit-trusted-cidr` 透传给 LoginApp 启动参数 —— 因为所有本地 client 共享 `127.0.0.1`，默认 5/60s 的 per-IP 限额撑不起 50 个 client 并发登录：

```bash
python tools/cluster_control/run_world_stress.py \
    --clients 50 --account-pool 50 \
    --duration-sec 30 \
    --script-clients 2 \
    --script-verify \
    --login-rate-limit-per-ip 200 \
    --login-rate-limit-trusted-cidr 127.0.0.0/8
```

（已验证 2026-04-22，`ce927b6..05f527d` 分支）退出时 exit=0，summary：

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

**读数含义（30 秒运行 + 50 bare + 2 script，所有实体均在同一 50 m AoI 半径）**：

| 列 | 期望 | 说明 |
|---|---|---|
| `init` | 52-54 | 1 Account + 52 StressAvatar peer（50 bare + 对端 script + 本端 script；self 也计 OnInit） |
| `enterworld` | 51-52 | 非 self 的 peer enter 才触发（self 的 StressAvatar 走 EntityTransferred → `CreateEntity` 路径，未经 `kEntityEnter`）|
| `hp` | 800-1000 | 52 avatars × 30 s × 1 Hz ≈ 1560 个全量事件；AoI 进出、优先级回退、带宽预算会裁剪一部分；稳态稀疏约 60% 落地 |
| `posupd` | 50-200 | 每个 bare 10 Hz × 30 s = 300；script client 看到的只有 AoI 半径内的 peer |
| `seqgaps` | **0** | 无丢包时 reliable delta 的 `event_seq` 必须连续；非零 = reliable channel 或 replication 路径有回归 |
| `unparsed` | ≈ 16 | init 阶段的 ClR / 网络日志行，非脚本事件 |

`--script-verify` 要求每个子进程至少观察到 1 次 `OnInit`，否则 exit code = 1。

## C3：失联恢复

C3 在 C2 基础上加一段丢包窗口，现在有**两种注入点**：

| Flag | 注入层 | 时机 | 适用场景 |
|---|---|---|---|
| `--drop-inbound-ms <s> <d>` | 应用层（`client_app.cc::MainLoop` 默认 handler） | RUDP 已 ACK 之后 | 验证 app 层的 gap 检测 / baseline 静默恢复路径（C3-A / C3-B） |
| `--drop-transport-ms <s> <d>` | 传输层（`ReliableUdpChannel::OnDatagramReceived`） | ACK 生成之前 | 验证 RUDP reliable 重传路径（发送方超时 → 重传 → 全部事件补齐） |

前者下 reliable 不会被重传（transport 不知道包丢了），所以 `OnHpChanged` 计数会少掉 drop window 里的数量；**后者下 reliable 传输会自动重试**，长期运行 `seqgaps` 归零。

`--drop-inbound-ms` 与 `--drop-transport-ms` 可以组合使用 —— 前者影响应用层决定是否丢弃已 ACK 的消息，后者影响传输层是否丢弃整个 datagram。实测时通常只开一个。

### C3-A：Reliable delta 路径 + baseline 静默恢复

只用 script client（双 client 互为 peer），在第 5-9 秒入站丢弃 state-channel 消息：

```bash
python tools/cluster_control/run_world_stress.py \
    --clients 0 --duration-sec 20 \
    --script-clients 2 \
    --script-verify \
    --client-drop-inbound-ms 5000 4000
```

已验证 2026-04-22：exit=0，
```
[script-clients] per-child event summary:
  username          init  enterworld  destroy  hp  posupd  seqgaps  unparsed
  script_user_1        3           1        0   31       1        8        17
  script_user_2        3           1        0   31       1        8        17
```

**不变量**：`hp + seqgaps ≈ duration_sec × avatars × 1 Hz`（上例 `31 + 8 = 39 ≈ 40`）。`hp` 是脚本层真正触发的 `OnHpChanged` 数，`seqgaps` 是 `event_seq` 跳号累计；二者之和对应服务端实际产生的 HP delta 总数。两个子进程 `seqgaps` 差值通常 ≤ 1（受窗口对 tick 边界对齐影响）。

### C3-B：unreliable 属性 + baseline 恢复

把 `StressAvatar.hp` 标成 `reliable="false"`，跑 C3-A 相同的 drop-window 场景，观察 L4 baseline pump 能否让 `_hp` 字段最终一致。一键 runner 包装了 patch → rebuild → smoke → restore，`try/finally` 保证 def 退出时一定回到 `reliable="true"`：

```bash
python tools/cluster_control/run_unreliable_recovery.py              # 默认 20s / drop 5..9s
python tools/cluster_control/run_unreliable_recovery.py --duration-sec 30
```

跑出来的 `hp / seqgaps` 和 C3-A 几乎相同（因为 `--client-drop-inbound-ms` 是**应用层**过滤，不区分 reliable / unreliable）。**差异在字段层面**：L4 baseline（每 ~6 s）静默把 `_hp` 拉回服务端真值，脚本看不到补调的事件（[`PROPERTY_SYNC_DESIGN.md §5.1b`](PROPERTY_SYNC_DESIGN.md) 解释为什么）；C3-A 下 reliable channel 由 transport 层保证，无需 baseline。

## 日志格式契约

`samples/client/StressAvatar.cs` 的日志形态被 `src/tools/world_stress/client_event_tap.cc` 按字面量解析。

| 脚本日志 | Tap 匹配 | 触发通道 |
|---|---|---|
| `[StressAvatar:42] OnInit` | `on_init` | 工厂刚创建实例（enter 或玩家自身） |
| `[StressAvatar:42] OnEnterWorld pos=(..) hp=..` | `on_enter_world` | `kEntityEnter` 信封初始快照应用完毕 |
| `[StressAvatar:42] OnDestroy` | `on_destroy` | `kEntityLeave` 或登出 |
| `[StressAvatar:42] OnHpChanged old=X new=Y` | `on_hp_changed` | **只由 delta 通道触发**（`0xF001` / `0xF003` kEntityPropertyUpdate → `ApplyReplicatedDelta`）；**baseline `0xF002` 不触发**（`ApplyOwnerSnapshot` 直写字段） |
| `[StressAvatar:42] OnPositionUpdated pos=(..)` | `on_position_updated` | volatile delta `0xF001` kEntityPositionUpdate |
| `[StressAvatar:42] event_seq gap: last=A got=B missed=N` | `event_seq_gaps += N` | 客户端检测到 `kEntityPropertyUpdate` 的 u64 seq 前缀跳号（D2'.2） |

修改日志格式时两边同步更新，否则 `unparsed_lines` 会飙升且对应计数会失准。

## 注意事项

- `atlas_client.exe` 的脚本事件走 `Console.WriteLine` → 子进程 stdout → world_stress 解析。压测规模 ≥ 几十 script-client 时管道会成为瓶颈，此时用 `--clients N`（裸协议）做负载、`--script-clients 2` 做观测点即可。
- `--drop-inbound-ms` / `--drop-transport-ms` 可同时开（应用层先丢、传输层再丢），但通常只开一个；想验证 reliable 重传用 transport 版本，想验证 gap 检测 / baseline 路径用 inbound 版本。
