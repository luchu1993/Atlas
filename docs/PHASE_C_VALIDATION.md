# Phase C 验证手册

> 日期: 2026-04-22（BigWorld 对齐 + Known Limitations 清零）
> 关联: [PROPERTY_SYNC_DESIGN.md §8.7 / §9.5 / §9.6](PROPERTY_SYNC_DESIGN.md) | [DEF_GENERATOR_DESIGN.md §10](DEF_GENERATOR_DESIGN.md)

## 基线结果快照（2026-04-22 跑一遍完整 sweep）

4 个场景的典型输出。偏差 ≤±2 属正常 tick 边界抖动，偏差更大再复查。

| 场景 | 命令（简化）| init | enterworld | hp | posupd | seqgaps |
|---|---|---:|---:|---:|---:|---:|
| **C2** 50 bare + 2 script, 30s | `... --clients 50 --duration-sec 30` | 53 | 51 | ~669 | ~84 | **0** |
| **C3-A** 2 script, 20s, app drop 5..9s | `... --client-drop-inbound-ms 5000 4000` | 3 | 1 | 31 | 1 | 8 |
| **§4** 2 script, 20s, **transport** drop 5..9s | `... --client-drop-transport-ms 5000 4000` | 3 | 1 | **39** | 1 | **0** |
| **C3-B** reliable=false + app drop 5..9s, 20s | `python tools/cluster_control/run_phase_c3b.py` | 3 | 1 | 31-33 | 1 | 6-8 |

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

`hp + seqgaps = 39 ≈ 40`（20 s × 2 avatars × 1 Hz），证明 event 总数正确 —— 8 个 reliable delta 在 drop window 被应用层丢弃、触发 8 个 `event_seq` 跳号、其余 31 个按 1 Hz 正常送达脚本。

**预期 summary**：

```
[script-clients] per-child event summary:
  username              init  enterworld  destroy  hp  posupd   seqgaps  unparsed
  script_user_X            1           1        0  ~16   ~200       ~4        ~30
  script_user_Y            1           1        0  ~16   ~200       ~4        ~30
```

- `hp` 大约 16（20s 运行 - 4s 丢失窗口，服务端 1 Hz 的 20 次里客户端真正触发回调的只有 ~16 次）
- `seqgaps` 约为 4：窗口内被丢的 4 个 reliable delta 在下次 envelope 到达时触发一次 "gap"，`missed=4` 累加到计数
- `_hp` 字段最终值应与 server 当前值一致（baseline 同步），但脚本层面 `OnHpChanged` 不会为丢失的那 4 个事件补调

**如何复核脚本层事件数与服务端发出的 delta 对得上**：
- `hp` + `seqgaps` ≈ 服务端 20s 内产出的 HP delta 总数（1 Hz × 20s = 20）
- 两个子进程 `seqgaps` 差值 ≤ 1（受窗口对 tick 边界对齐影响）

### C3-B：纯 baseline 恢复（可选变体）

关闭 reliable 属性后再跑同一命令，观察 L4 baseline（commit `f2dec1e`：CellApp 侧 pump、每 120 tick ≈ 6 s 发一次 `ReplicatedBaselineFromCell` 经 BaseApp 转 0xF002 给 client）能否让 `_hp` 字段最终一致。

**一键流程**（`tools/cluster_control/run_phase_c3b.py` 包装了 patch / rebuild / smoke / restore 的 try-finally 序列，`--skip-restore` 之外保证退出时 def 一定回到 `reliable="true"`）：

```bash
python tools/cluster_control/run_phase_c3b.py              # 默认 20s / drop 5..9s
python tools/cluster_control/run_phase_c3b.py --duration-sec 30
```

底层等价命令（debug 或分步调试时）：

1. 编辑 `entity_defs/StressAvatar.def`，把 `hp` 属性的 `reliable="true"` 改为 `reliable="false"`
2. 重建受影响的 dll / exe：
   ```bash
   cmake --build build/debug --config Debug --target \
       atlas_stress_test_cell_dll atlas_stress_test_base_dll \
       atlas_baseapp atlas_cellapp atlas_client atlas_client_desktop_dll
   dotnet build samples/client/Atlas.ClientSample.csproj --no-incremental --nologo
   ```
3. 跑 **C3-A 的完全相同命令行**
4. **验证完改回** `reliable="true"` + rebuild —— 日常 stress / 测试依赖 reliable delta

已验证 2026-04-22（L4 + reliable=false）：

| 场景 | summary | 说明 |
|---|---|---|
| C3-B 无 drop window, 20s | `hp=39 seqgaps=0` | 40 events × 97.5%：unreliable delta 在 127.0.0.1 回环上基本无丢；0 跳号 ≈ 理想情况 |
| C3-B drop 5..9s, 20s | `hp=31 seqgaps=8` | 与 C3-A 完全一致：8 个 delta 被应用层丢弃 → 8 跳号；其余 31 正常 |

**两点观察**：

1. **脚本事件计数没有差异**：因为 `--client-drop-inbound-ms` 是**应用层**过滤，不区分 reliable/unreliable；丢弃行为相同 → summary 数字相同。
2. **字段层面 L4 baseline 确实在工作**，但脚本看不到。B2 scheme-2 明确 baseline 不触发 `OnXxxChanged`（对齐 BigWorld `shouldUseCallback=false`），所以脚本只能从 `seqgaps` 推断"被吞了多少 event"，从下次 delta 的 `old` 值确认恢复（`old` 来自 apply 时字段当前值，baseline 先静默改 `_hp`，下条 delta 的 `old` 就是 baseline 写的值）。

**字段级复核（可选，定位 baseline 收敛时间）**：在 `StressAvatar.cs` 定期打 `Hp` 值（需要 `ClientEntity` 加 `OnTick` 虚方法）：

```csharp
protected override void OnTick(float dt) {
    if (++_tickCount % 30 == 0) {
        Console.WriteLine($"[StressAvatar:{EntityId}] HpSample value={Hp}");
    }
}
```

现场可用 `tools/cluster_control/run_world_stress.py --script-clients 1 --duration-sec 20 --client-drop-inbound-ms 5000 4000` 配上述扩展，观察 drop window 后 `HpSample` 的值从 drop-前值漂移到服务端当前值（6 s 内，下一次 baseline pump 周期）。

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

## 已知限制

1. ~~CLR Bootstrap 对客户端路径未就绪~~ **已修复** —— atlas_client 的 CLR 初始化链现在端到端 green：CoreCLR init → `Atlas.Core.Bootstrap, Atlas.ClrHost` → `DesktopBootstrap.Initialize` 填槽 + 注册 native callback table → `DesktopBootstrap.LoadUserAssembly` 把用户 dll 载入 **host 所在的 ALC**（不能用 `Assembly.LoadFrom` 或 `AssemblyLoadContext.Default`，否则 `Atlas.Client` 会被加载两份、generator 的 `ModuleInitializer` 注册到一份 factory 而 [UnmanagedCallersOnly] bridge 读另一份，`CreateEntity` 永远返回 null）并显式跑 `RunModuleConstructor` 触发注册。脚本钩子能正常点亮：`Account.OnInit → SelectAvatar → StressAvatar.OnInit / OnEnterWorld / OnPositionUpdated` 按序触发。
2. ~~HP delta 流尚未达到脚本回调层~~ **已修复** —— 问题不是 handoff bug，而是**三条路径级缺失**：
   - C# `Lifecycle.DoOnTick` 从不调 `BuildAndConsumeReplicationFrame`（无人消费脏位）
   - `NativeApi.cs` 缺 `AtlasPublishReplicationFrame` 的 `[LibraryImport]`（消费了也到不了 C++）
   - Witness 不追踪 owner 自己（`&peer == &owner_` 直接 return），导致 own-scope delta 无路可走。client 侧 `EntityTransferred` 也没创建对应 entity 实例。

   参考 BigWorld 的 own-scope 直发路径（`bigworld/server/cellapp/entity.cpp:5670` 的 `pWitness->sendToClient`）后的修法：
   - `Lifecycle.DoOnTick` 的 `OnTickAll` 之后遍历 entity 调 `BuildAndConsumeReplicationFrame` + `NativeApi.PublishReplicationFrame`
   - `CellAppNativeProvider::PublishReplicationFrame` 里把 owner_delta 打成 kEntityPropertyUpdate envelope，独立经 `ReplicatedReliableDeltaFromCell (2017)` → BaseApp → 0xF003 直发 client，绕开 witness.aoiMap
   - `client_app.cc` 的 `EntityTransferred` handler 调 native `CreateEntity(new_id, new_type)` 实例化 client 侧 entity

   验证：`--script-clients 2` 时每个 client 都看到 self + peer 两个 StressAvatar 的 HP 下降（15s → hp≈29，双倍节律）；`--script-clients 1` 时只看到 self（hp≈15，1 Hz 节律）。

3. **Baseline pump 暂时禁用（BigWorld 对齐已完成）**：`BaseApp::EmitBaselineSnapshots` 从 hotfix 起改为 no-op（commit `de42fc0`），中期 (M2, `46c70b9`) + 长期 (L1 `3fdbd2d`, L2 `788330d`) 工作已落地，对齐 BigWorld 的 base/cell 字段互斥 + cell→base opaque bytes 模型：
   - **M1** (`ce927b6`): `PropertyScope` 增加 `IsBase / IsCell / IsGhosted / IsOwnClientVisible / IsOtherClientsVisible / IsClientVisible` 谓词，单一权威来源；对齐 BigWorld `DataDescription::isCellData / isBaseData`。
   - **M2** (`46c70b9`): generator 按 side 分拆 field 生成。Base partial 只 emit `DATA_BASE` 属性 (`base` / `base_and_client`) 的字段；cell partial 只 emit cell-scope 的字段。消除了 "base 侧 `_hp` 永远是 0" 的歧义源。`ReplicatedDirtyFlags` enum 仍列出所有 `IsClientVisible` 属性（wire 一致），但 backing field 只在 owning side emit。
   - **L1** (`3fdbd2d`): 新增 `BackupCellEntity` (msg 2018, reliable) — CellApp 每 50 tick (~1s) 对每个 has-base entity 调 `SerializeEntity` 拿到 cell-scope bytes，发给 BaseApp。BaseApp 存入 `BaseEntity::cell_backup_data_` 不反序列化。对齐 BigWorld `bigworld/server/cellapp/real_entity.cpp:884-906`。
   - **L2** (`788330d`): DB blob 升级为 `[magic=0xA7][ver=1][base_len][base][cell_len][cell]`。`CaptureEntitySnapshot` 在写 DB 前把两段拼起来；checkout 时 `DecodeDbBlob` 拆回。CellApp 重建时 `CreateCellEntity.script_init_data` 使用检出的 cell 部分，保证 cell-scope `persistent="true"` 跨 DB 生命周期保留。

   **Reviver / Offload**：offload 路径（Phase 11 `BuildOffloadMessage` → `OnOffloadEntity`）原本就使用 `SerializeEntity` / `RestoreEntity`，M2 后那些字节已经是 cell-scope-only，没有额外改动需要。Reviver 是 placeholder (`src/server/reviver/CMakeLists.txt` 只有注释)，等实现时直接用 `cell_backup_data_` 就行。

   **L4 Baseline on CellApp**（commit `f2dec1e`）**已实施**：CellApp 侧新增 `TickClientBaselinePump`，每 `kClientBaselineIntervalTicks = 120` tick 对每个 has-witness entity 调 `GetOwnerSnapshot` 拿 cell-side `SerializeForOwnerClient` 输出，通过新消息 `ReplicatedBaselineFromCell` (msg 2019, reliable) 发给 BaseApp。BaseApp 的 `OnReplicatedBaselineFromCell` 只做中继：`ResolveClientChannel` → 发 `ReplicatedBaselineToClient` (0xF002) 给 owner client。payload 是 cell-side owner-scope 快照，wire 与 pre-M2 baseapp pump 输出 byte-identical，客户端解码零改动。C3-B 现已可验证（见 §C3-B）。
4. ~~Drop 过滤器在 RUDP 之上~~ **已新增 `--drop-transport-ms`**：`atlas_client --drop-transport-ms <start_ms> <duration_ms>` 在 `ReliableUdpChannel::OnDatagramReceived` 入口按时间窗口丢弃整包（在头部解析 + ACK 生成之前），与 application-layer `--drop-inbound-ms` 对照。`--drop-transport-ms` 下 reliable 包的丢失会触发发送方的 retransmit，reliable 流能完整恢复；`--drop-inbound-ms` 则只模拟"应用层漏接"（RUDP 已 ACK，不触发重传）。world_stress 加配套 `--client-drop-transport-ms`；python runner 加同名 flag。已验证 2026-04-22：transport drop 5-9s / 20s → `hp=39 seqgaps=0`（reliable 路径完整重传，对比 app drop 同场景 `hp=31 seqgaps=8`）。
5. **Baseline 不触发 `OnXxxChanged`**（BigWorld 对齐，不是 Atlas 限制）：B2 scheme-2 直接对齐 BigWorld 的 `isInitialising → shouldUseCallback=false` 契约 —— `client/entity.cpp:1124-1133` 把每次 property reset 传递的 `isInitialising` 翻成 `!shouldUseCallback` 交给 `simple_client_entity.cpp:135-160::propertyEvent`，后者在 `!shouldUseCallback` 下**直接写字段跳过 `set_<propname>` Python 回调**。

   | Atlas | BigWorld | 触发 On*Changed？ |
   |---|---|---|
   | `ApplyOwnerSnapshot` / `ApplyOtherSnapshot`（0xF002 baseline / `kEntityEnter` 初始快照）| `onProperty(isInitialising=true)` | ✗ |
   | `ApplyReplicatedDelta`（0xF001 / 0xF003 运行期 delta） | `onProperty(isInitialising=false)` | ✓ |

   所以脚本层"baseline 静默恢复、只有 delta 触发回调"是设计，不是局限。脚本如果必须观察到 baseline 带来的字段变化，对齐 BigWorld 的做法是**自己读字段值**（周期轮询 `_hp` 等）或**通过 `seqgaps` 推断被吞了多少 event**。
6. ~~Tap 不带时间戳~~ **已修复**：`Atlas.Client.ClientLog` 给每条脚本日志前置 `[t=S.sss]` 单调秒戳（起点为首次 ClientLog 访问；进程启动后几 ms 内即激活）。`client_event_tap.cc::EventBegins` 新增前缀剥离，计数语义向后兼容。后续收敛分析可用 `grep '^\[t=\([0-9.]*\)\]'` 把时间戳抽回来对 drop window 边界做自动断言。
7. ~~C3-B 需手动 `.def` 改动 + rebuild~~ **已缓解**：`tools/cluster_control/run_phase_c3b.py` 把 patch → rebuild → smoke → restore 整合成一条命令，用 `try/finally` 保证 def 退出时一定回到 `reliable="true"`（除非显式传 `--skip-restore`）。运行时切换的 generator-level 开关仍未做，但"一键完整 round-trip"已经消除手动漏步回退的风险。
8. **子进程日志冗余**：脚本走 `Console.WriteLine`；大流量场景下 stdout 管道可能成为瓶颈。压测规模 > 几十 client 时建议关闭 `--script-clients`，用裸协议 path。
