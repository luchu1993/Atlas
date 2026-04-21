# Phase C 验证手册

> 日期: 2026-04-21
> 关联: [PROPERTY_SYNC_DESIGN.md §8.7 / §9.5 / §9.6](PROPERTY_SYNC_DESIGN.md) | [DEF_GENERATOR_DESIGN.md §10](DEF_GENERATOR_DESIGN.md)

Phase C 把 Phase B 的客户端回调契约放到真实集群里跑一遍。本文档覆盖两条验证链路：

- **C2（事件可达）**：服务端属性/位置变更 → 客户端脚本钩子触发
- **C3（失联恢复）**：客户端在窗口内丢失入站包，窗口结束后状态最终一致

两条链路都复用同一套工具 —— `world_stress` 的 `--script-clients` 模式启动真实的 `atlas_client.exe` 子进程，通过 stdout 解析 `[StressAvatar:<id>] <Event>` 日志行计数事件。所有必要的脚本钩子由 `samples/client/StressAvatar.cs` 提供，服务端 HP 节律由 `samples/stress/Atlas.StressTest.Cell/StressAvatar.cs::OnTick` 驱动（1 Hz，100 → 1 → 100 循环）。

## 先决条件

- 集群跑在 `127.0.0.1`：`machined` + `LoginApp` + `BaseApp` + `CellApp` + `DBApp`
- `atlas_client.exe` 已构建（`build/debug/src/client/Debug/atlas_client.exe`）
- `Atlas.ClientSample.dll` 已构建（`samples/client/bin/.../Atlas.ClientSample.dll`）
- `entity_defs/StressAvatar.def` 的 `hp` 属性为 `reliable="true"`（默认值）

## C2：事件可达链路

一次命令同时启动 50 个裸协议虚拟客户端（负载）+ 2 个真实脚本客户端（观察）：

```bash
world_stress --login 127.0.0.1:20013 \
             --password-hash <sha256hex> \
             --clients 50 --account-pool 50 \
             --duration-sec 30 \
             --script-clients 2 \
             --client-exe build/debug/src/client/Debug/atlas_client.exe \
             --client-assembly samples/client/bin/x64/Debug/net9.0/Atlas.ClientSample.dll \
             --script-verify
```

退出时打印 per-child summary：

```
[script-clients] per-child event summary:
  username              init  enterworld  destroy  hp  posupd  unparsed
  script_user_50           1           1        0   27     295       41
  script_user_51           1           1        0   27     295       42
```

**预期读数（30 秒运行）**：

| 列 | 含义 | 期望 |
|---|---|---|
| `init` | `OnInit` 次数（自身实体创建） | ≥ 1 |
| `enterworld` | `OnEnterWorld` 次数（自身 + AoI peer） | ≥ 1（AoI 有同空间 peer 时会 >1） |
| `hp` | `OnHpChanged` 次数（服务端 HP tick 触发） | ≈ 运行秒数（1 Hz 节律）|
| `posupd` | `OnPositionUpdated` 次数 | 由 `--move-rate-hz` 决定（默认 10 Hz × 30 s = 300 附近） |

`--script-verify` 要求每个子进程至少观察到 1 次 `OnInit`，否则 exit code = 1。

## C3-A：Reliable 重传（补强四）

服务端 HP 以 1 Hz 变化；客户端从 T=5s 开始丢失入站 state-channel 包持续 4 秒（missing 4 HP updates）；窗口结束后 reliable 重传应把这 4 个 delta 补齐。

```bash
world_stress --login 127.0.0.1:20013 \
             --password-hash <sha256hex> \
             --clients 0 --duration-sec 20 \
             --script-clients 2 \
             --client-exe build/debug/src/client/Debug/atlas_client.exe \
             --client-assembly samples/client/bin/x64/Debug/net9.0/Atlas.ClientSample.dll \
             --client-drop-inbound-ms 5000 4000
```

**预期 summary**：两个子进程的 `hp` 计数都接近运行总秒数（~18-20 次），差值 ≤ 2。**关键**：丢包窗口内产生的 HP 事件不会丢失，只是推迟到 T=9s 后集中涌入客户端脚本。

**如何复核**：grep 子进程 stdout 中 `OnHpChanged` 的时间戳分布（需要额外打时间戳，当前脚本只打 old/new 值）。更严格的复核请加时间戳后重跑。

## C3-B：Baseline 独立恢复（补强一）

独立验证 baseline 能否在 reliable 失效时兜底。**需要一次手工 .def 改动 + rebuild**：

1. 编辑 `entity_defs/StressAvatar.def`，把 `hp` 属性的 `reliable="true"` 改为 `reliable="false"`
2. 重建受影响的工程：
   ```bash
   cmake --build build/debug --config Debug --target world_stress \
         samples/stress/Atlas.StressTest.Cell \
         samples/client/Atlas.ClientSample
   ```
3. 跑 **C3-A 的完全相同命令行**

**预期 summary**：
- 丢包窗口内被 miss 的 HP 事件**不会重传**（reliable 关掉了）
- 但是每 `kBaselineInterval * tick_ms` 的 baseline 0xF002 仍然下发 owner-scope 全量快照
- 客户端 HP 状态最终收敛（脚本 `OnHpChanged` 计数会**少几次**，但最后一次 `OnHpChanged` 的 `new=` 值与无丢包 child 一致）

**复核方式**：grep 两个子进程各自最后一次 `OnHpChanged` 的 `new=` 值，应相等或差距不超过 1（取决于窗口对 tick 边界的对齐）。

记得**验证完改回** `reliable="true"` —— 日常 stress / 测试依赖 reliable delta。

## 日志格式契约

`samples/client/StressAvatar.cs` 的日志形态被 `src/tools/world_stress/client_event_tap.cc` 按字面量解析。

| 脚本日志 | Tap 匹配 | 说明 |
|---|---|---|
| `[StressAvatar:42] OnInit` | `on_init` | 工厂刚创建实例 |
| `[StressAvatar:42] OnEnterWorld pos=(..) hp=..` | `on_enter_world` | 初始 transform + snapshot 已应用 |
| `[StressAvatar:42] OnDestroy` | `on_destroy` | AoI leave / 登出 |
| `[StressAvatar:42] OnHpChanged old=X new=Y` | `on_hp_changed` | reliable delta 0xF003（也可能 baseline 触发）|
| `[StressAvatar:42] OnPositionUpdated pos=(..)` | `on_position_updated` | volatile delta 0xF001 |

修改日志格式时两边同步更新，否则 `unparsed_lines` 会飙升且对应计数会失准。

## 已知限制

1. **Tap 不带时间戳**：无法自动断言"窗口后 4s 内收敛"。需要 in-script timestamp 才能机械化；当前脚本只打 old/new 值。
2. **C3-B 需手动 .def 改动 + rebuild**：没有运行时 reliable 开关。未来可加 `.def` 的"条件 reliable"支持或 CLI 覆盖，现阶段"改 def → rebuild → 测 → 改回"流程可接受。
3. **子进程日志冗余**：脚本走 `Console.WriteLine`；大流量场景下 stdout 管道可能成为瓶颈。压测规模 > 几十 client 时建议关闭 `--script-clients`，用裸协议 path。
