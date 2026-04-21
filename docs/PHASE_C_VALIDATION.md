# Phase C 验证手册

> 日期: 2026-04-21（D 阶段完成后校正）
> 关联: [PROPERTY_SYNC_DESIGN.md §8.7 / §9.5 / §9.6](PROPERTY_SYNC_DESIGN.md) | [DEF_GENERATOR_DESIGN.md §10](DEF_GENERATOR_DESIGN.md)

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

一次命令同时启动 50 个裸协议虚拟客户端（负载）+ 2 个真实脚本客户端（观察）：

```bash
world_stress --login 127.0.0.1:20013 \
             --password-hash <sha256hex> \
             --clients 50 --account-pool 50 \
             --duration-sec 30 \
             --script-clients 2 \
             --client-exe build/debug/src/client/Debug/atlas_client.exe \
             --client-assembly samples/client/bin/Debug/net9.0/Atlas.ClientSample.dll \
             --script-verify
```

退出时打印 per-child summary（D2'.3 起多一个 `seqgaps` 列）：

```
[script-clients] per-child event summary:
  username              init  enterworld  destroy  hp  posupd   seqgaps  unparsed
  script_user_50           1           1        0  ~30   ~300        0        ~40
  script_user_51           1           1        0  ~30   ~300        0        ~40
```

**预期读数（30 秒运行）**：

| 列 | 含义 | 期望 |
|---|---|---|
| `init` | `OnInit` 次数（自身实体创建 / AoI peer 进入） | ≥ 1 |
| `enterworld` | `OnEnterWorld` 次数 | ≥ 1 |
| `hp` | `OnHpChanged` 次数（服务端 HP tick 触发，**仅 delta 通道**） | ≈ 运行秒数（1 Hz 节律）|
| `posupd` | `OnPositionUpdated` 次数 | 由 `--move-rate-hz` 决定（默认 10 Hz × 30 s = 300 附近） |
| `seqgaps` | 客户端观察到的 `event_seq` 跳号数（missed deltas） | **0**（无丢包时应完全连续） |

`--script-verify` 要求每个子进程至少观察到 1 次 `OnInit`，否则 exit code = 1。

## C3：失联恢复

C3 在 C2 基础上加一段丢包窗口 —— 借 `atlas_client --drop-inbound-ms <start> <duration>` 让一个（或全部）脚本子进程在指定时间段静默丢弃入站的 state-channel 消息（`0xF001` / `0xF002` / `0xF003`）。窗口外流量正常。

**⚠ 关键限制（请先读）**：drop 过滤器跑在 **RUDP 已经 ACK 之后的应用层**。对发送端来说消息"已送达"，传输层不会触发重传。因此当前的 `--drop-inbound-ms` 仅**模拟"应用层漏接"**，**并不能**完整验证"reliable 重传补齐"路径 —— 做到传输层级的丢包注入需要改动 RUDP（留作后续工作）。

这意味着 C3-A 与 C3-B 实际测试的都是同一个机制：**丢失的属性 delta 不再通过 `OnHpChanged` 触达脚本，窗口结束后 `0xF002` baseline 把 `_hp` 字段静默同步到服务端当前值**。B2 scheme-2 明确规定 baseline 不触发 `OnXxxChanged`（对齐 BigWorld `shouldUseCallback=false`），所以 baseline 恢复对脚本是"数据到位、通知缺席"。

### C3-A：Reliable delta 路径 + baseline 静默恢复

配置与 C2 相同但加一段 5-9 秒的入站丢弃：

```bash
world_stress --login 127.0.0.1:20013 \
             --password-hash <sha256hex> \
             --clients 0 --duration-sec 20 \
             --script-clients 2 \
             --client-exe build/debug/src/client/Debug/atlas_client.exe \
             --client-assembly samples/client/bin/Debug/net9.0/Atlas.ClientSample.dll \
             --client-drop-inbound-ms 5000 4000
```

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

关闭 reliable 属性后再跑同一命令，观察 baseline 能否在更苛刻的场景（unreliable delta 通道，丢失真正"不可追"）下同样让 `_hp` 字段最终一致。**需要一次手工 `.def` 改动 + rebuild**：

1. 编辑 `entity_defs/StressAvatar.def`，把 `hp` 属性的 `reliable="true"` 改为 `reliable="false"`
2. 重建受影响的工程：
   ```bash
   cmake --build build/debug --config Debug --target world_stress \
         samples/stress/Atlas.StressTest.Cell \
         samples/client/Atlas.ClientSample
   ```
3. 跑 **C3-A 的完全相同命令行**

**预期 summary 形态与 C3-A 近似**：`hp` 计数相近、`seqgaps` 相近。差异在**失联窗口结束后下一个 baseline 到达前**的 `_hp` 字段值：对 unreliable 通道，丢失的 delta 本来就不会重传；只有 baseline 能把 `_hp` 拖回真值。baseline 在 `kBaselineInterval = 120` tick ≈ 4s (30 Hz) / 12s (10 Hz) 发一次，所以收敛时间可能比 C3-A 略长。

由于脚本只看事件不看字段值，summary 里 `hp` 计数差异**不明显**。更严格的字段级复核需要在 `StressAvatar.cs` 定期打 `Hp` 值：

```csharp
// 可选扩展（不在当前脚本中）
protected override void OnTick(float dt) {  // 需要 ClientEntity 加 OnTick 虚方法
    if (++_tickCount % 30 == 0) {
        Console.WriteLine($"[StressAvatar:{EntityId}] HpSample value={Hp}");
    }
}
```

记得**验证完改回** `reliable="true"` —— 日常 stress / 测试依赖 reliable delta。

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

1. **CLR Bootstrap 对客户端路径未就绪（当前 blocker）**：`src/lib/clrscript/clr_bootstrap.cc` 固定查找 `Atlas.Core.Bootstrap, Atlas.Runtime` 作为 CLR 入口；但 `samples/client/Atlas.ClientSample.csproj` 只引用 `Atlas.Client.dll` / `Atlas.Shared.dll`（服务端专属类型不应混进客户端资产），运行时 hostfxr 找不到 `Atlas.Runtime` → 报 `Failed to load method 'Atlas.Core.Bootstrap, Atlas.Runtime.Initialize'` 后退出。C2 / C3 **端到端完整跑通前**需把 `Bootstrap` / `ErrorBridge` / `GCHandleHelper` 等 host-agnostic 部分从 `Atlas.Runtime` 迁到 `Atlas.Shared`（或新建 `Atlas.ClrHost` 之类的中立 dll）然后让客户端和服务端都引用。当前手册的脚本客户端命令**能跑通到 CoreCLR init**，但在 Bootstrap.Initialize 查表那一步失败 —— 单元测试 / CLI parse / ChildProcess harness / stdout tap 等全链路都已验证，仅这一步缺。
2. **Drop 过滤器在 RUDP 之上**（见 C3 开头警告）：当前 `--drop-inbound-ms` 不能验证传输层 reliable 重传；C3-A 和 C3-B 实际测试的都是"应用层漏接 + baseline 静默恢复"。要真正验证 RUDP 重传需要在 `src/lib/network/reliable_udp.cc` 加丢包注入点（未来工作）。
3. **Baseline 不触发 `OnXxxChanged`**：B2 scheme-2 明确选择 baseline 静默路径。脚本看不到被 baseline 恢复的字段变化，只能轮询字段值或依赖 `seqgaps` 推断"被吞了多少"。
4. **Tap 不带时间戳**：无法自动断言"窗口后 4s 内收敛"。当前脚本只打 old/new 值；要机械化需要给每条日志前置 server-time 戳。
5. **C3-B 需手动 `.def` 改动 + rebuild**：没有运行时 reliable 开关。未来可加 `.def` 的"条件 reliable"或 CLI 覆盖，现阶段"改 def → rebuild → 测 → 改回"流程可接受。
6. **子进程日志冗余**：脚本走 `Console.WriteLine`；大流量场景下 stdout 管道可能成为瓶颈。压测规模 > 几十 client 时建议关闭 `--script-clients`，用裸协议 path。
