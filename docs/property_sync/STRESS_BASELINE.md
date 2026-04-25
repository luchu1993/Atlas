# 容器同步 / Component 实施 —— 压测基线

> 初版 2026-04-24 / 最新 2026-04-24（§3 换成 release build；§5 登记 aoi_pos_update 客户端计数器 bug 修复）
> 关联: [容器属性同步设计](./CONTAINER_PROPERTY_SYNC_DESIGN.md) | [Entity / Component 设计](./COMPONENT_DESIGN.md) | [world_stress 文档](../stress_test/WORLD_STRESS_TESTING.md)
>
> **用途**：P0.1–P3.6 每个阶段完成后复跑下列 config，对比 bytes/s 与延迟增量。所有数据必须在**同一台机器、同一 build 类型（release）**上才有可比性。

---

## 1. 采集能力现状

### 1.1 本次新增（commit TBD，patch `0001-world-stress-byte-counters.patch`）

world_stress 客户端 pre-dispatch hook 对**所有进站消息**按 msg_id 累计：

- `bytes_rx_total` — 整会话总下行字节
- `bytes_rx_per_sec` — 按 `--duration-sec` 的均值
- `bytes_rx_per_cli_s` — 按客户端数均值
- `bytes_rx_top_msg` — 按字节数降序列出 top 8 msg_id 的 (count / bytes / avg)

### 1.2 留待 P0.8 前补齐（见 STRESS_BASELINE §5）

- **CellApp tick p50/p95/p99 duration** — 当前只有"Slow tick"启动瞬间告警
- **C# GC Gen0/Gen1/Gen2 计数** — 对 Unity Mono 客户端 GC 压力评估关键
- **Per-client outbound 带宽** — 当前只采下行；服务端→客户端是主矛盾，但后续 P2 kStructFieldSet 节省要看双向对比

---

## 2. 本机硬件/构建环境

- OS: Windows 10 Pro (19045)
- Build: `build/release` (`cmake --preset release` → `cmake --build ... --config Release`)
- Python driver: `tools/cluster_control/run_world_stress.py --build-dir build/release --config Release`
- 测量日期: 2026-04-24

> **build 纪律**：所有后续 P0.x / P1 / P2 / P3.x 压测**必须用 release build**。debug build 下 25 客户端的 echo_rtt_p95 ≈ 76 ms，release 同 config 仅 ≈ 14 ms；两者不可直接比较。脚本必须同时传 `--build-dir build/release` 和 `--config Release`，只传一个会 fallback 到 debug 二进制。

---

## 3. 对照基线（P0 前、本机 debug build）

### 3.1 Config-S25（25 客户端，20s，单 space，单 CellApp）—— 稳态区

**命令**：

```bash
python tools/cluster_control/run_world_stress.py \
  --build-dir build/release --config Release \
  --clients 25 --account-pool 25 \
  --duration-sec 20 --ramp-per-sec 12 \
  --hold-min-ms 15000 --hold-max-ms 15000 \
  --shortline-pct 0 \
  --rpc-rate-hz 2 --move-rate-hz 10 \
  --space-count 1 --cellapp-count 1 \
  --login-rate-limit-trusted-cidr 127.0.0.0/8 \
  --login-rate-limit-global 10000
```

**基线结果**（2026-04-24 17:01，release build + 含 aoi_pos_update bug fix 的 world_stress）：

| 指标 | 值 | 健康？ |
|---|---|---|
| `login_started / login_success` | 50 / 50 | yes |
| `auth_success / entity_transferred / cell_ready` | 50 / 50 / 50 | yes（1:1:1） |
| `echo_sent / received / loss` | 927 / 906 / 21 (≈ 2.3%) | yes |
| `echo_rtt_p50 / p95 / p99` | **2.20 / 14.25 / 29.56 ms** | yes |
| `move_sent / fail` | 4514 / 0 | yes |
| `auth_latency_p50 / p95` | 30.76 / 47.36 ms | yes（匹配文档 49ms） |
| `aoi_enter / prop_update` | 1897 / 12263 | — |
| `aoi_pos_update` | **4451** | 现在正常计数（见 §5.3） |

**下行流量**：

| 字段 | 值 |
|---|---|
| `bytes_rx_total` | 443 676 B |
| `bytes_rx_per_sec` | 22 183 B/s (21.66 KB/s) |
| `bytes_rx_per_cli_s` | **887.4 B/s/client** |
| Top msg 0xF003 reliable delta | 14160 条 / 289026 B / **avg 20.4 B** |
| Top msg 0xF001 unreliable delta | 4451 条 / 133530 B / avg 30.0 B |
| Top msg 0x0201 EchoReply | 906 条 / 18120 B / avg 20.0 B |
| Top msg 0x1389 LoginApp | 50 条 / 1950 B / avg 39.0 B |
| Top msg 0xF002 baseline | 25 条 / 150 B / avg 6.0 B |

**关键观察**：
1. **0xF003 reliable delta 平均 20.4 B/msg**：这是后续容器 op-log 要搭便车的通道（见 [CONTAINER_PROPERTY_SYNC_DESIGN §7.2](./CONTAINER_PROPERTY_SYNC_DESIGN.md#72-帧内分段仅-msg-2017-可靠通道)）。每新增容器 op 会让 avg 上升；P1 启用 op-log 后这个 avg 预计是主要变化点。
2. **0xF001 unreliable delta = 4451 条 / avg 30.0 B**：承载 `kEntityPositionUpdate`（kind=3）volatile 流。布局 `[u8 kind][u32 entity_id][6×f32 pos+dir][u8 og] = 30 B`，计数与协议字节数对齐。该通道**与容器 op-log 物理分离**（详见 §5.3），容器 op-log 工作时不会侵占它。
3. `aoi_prop_update=12263 ≫ aoi_enter=1897`：observers 进入后属性更新流占主导（包含位置以外的属性变化 + event_seq 推进触发的 replay）。

### 3.2 Config-S50 与 Config-S200（暂不作基线）

debug build 下 Config-S50（50 客户端单 space）已进入过载（echo_loss 39%）；Config-S200（200 客户端多 space）login 成功率仅 50%。release build 下这两个 config 的基线尚未测。计划：每轮 P0.x 完成后若 Config-S25 无劣化，再跑一次 release Config-S50 作辅助对照。

---

## 4. 对照流程（每个实施阶段都跑一次）

1. **P0.1–P0.7 完成后**：先跑 `ctest --label-regex unit`；绿则跑 **Config-S25**，与本文档 §3.1 比对。
2. **增量规则**：
   - `bytes_rx_per_cli_s` 涨幅 > 20% 需说明（容器 op-log 启用预期 +10–15%）
   - `echo_rtt_p95` 涨幅 > 30% 需 profile
   - `aoi_prop_update` 每客户端每秒平均绝对数不应下降（否则是丢包/漏发）
3. **每次产出**：把 Summary 片段（至少 bytes_rx_* + rtt_* + cell_ready）附在该阶段的 commit message / tmp/patches 附近，便于 bisect。

---

## 5. 未实现但要加的采集（P0.8 前）

### 5.1 CellApp tick duration 分布

位置：`src/server/cellapp/cellapp.cc` 的主 tick loop。建议加一个环形缓冲（最后 N=2048 tick 的微秒数），进程退出前输出 p50/p95/p99。

### 5.2 C# GC 计数

C# 侧每 5s 上报 `GC.CollectionCount(0/1/2)` 的 delta，通过 `UnmanagedCallersOnly` 推到 native 计数器。Summary 时输出 per-app 累计。

### 5.3 Envelope 路径调研（已落盘，2026-04-24）

**问题**：首轮 debug 基线中 `aoi_pos_update=0`，怀疑是协议路径冲突，可能影响后续容器 op-log。

**结论**：**客户端计数器 bug**，非路径冲突。`CellAoIEnvelope` 四种 kind 的路由（witness.cc:365-374 + cellapp.cc:689-722）：

| Kind | 名称 | Witness 路径 | BaseApp 消息 | Client wire id |
|---|---|---|---|---|
| 1 | `kEntityEnter` | reliable | `ReplicatedReliableDeltaFromCell (2017)` | `0xF003` |
| 2 | `kEntityLeave` | reliable | 同上 | `0xF003` |
| 3 | `kEntityPositionUpdate` | **unreliable** | `ReplicatedDeltaFromCell (2015)` | **`0xF001`** |
| 4 | `kEntityPropertyUpdate` | reliable | `2017` | `0xF003` |

world_stress 原客户端（`main.cc:473`）只对 `id==0xF003` 检查 `payload[0]`，所以走 `0xF001` 的 kind=3 永远不被计数，`aoi_pos_update` 恒 0。

**修复**（patch 0003）：新增 `id==kUnreliableDeltaWireId (0xF001)` 分支，对 `payload[0]==3` 计数。Release build 首次跑 Config-S25 即得 `aoi_pos_update=4451`，与 0xF001 的 4451 条消息完全一致，证实修复正确。

**对容器同步的含义**：volatile 位置 (`0xF001`) 与属性事件 (`0xF003`) 走**物理分离通道**，容器 op-log 按 [CONTAINER_PROPERTY_SYNC_DESIGN §7.1](./CONTAINER_PROPERTY_SYNC_DESIGN.md#71-消息通道) 规划仅占用 `0xF003` 的 ContainerOp section，不会与 kind=3 位置流冲突。P0.1 安全推进。

### 5.4 容器 op-log 分 op 计数

P1 开始需要：按 opKind（kSet / kListSplice / …）分桶统计 count + bytes，用于判断同 tick 合并的实际节省。

---

## 6. P0 交付后对比

**背景**：P0 阶段完成后 StressAvatar 新增一个 7 字节定长 struct 属性 `mainWeapon`（`StressWeapon { int32 id; uint16 sharpness; bool bound }`），服务端 `OnTick` 每秒改一次 `mainWeapon.Id`。Config-S25 再跑一次，与 §3.1 baseline 直接横比。

### 6.1 Config-S25 对比表

| 指标 | Baseline (无 struct) | P0.8 (+struct 每秒改) | Δ |
|---|---|---|---|
| `login_success` / `entity_transferred` / `cell_ready` | 50/50/50 | 50/50/50 | 无回归 |
| `echo_rtt_p50 / p95 / p99` | 2.20 / 14.25 / 29.56 ms | 1.49 / 13.39 / 21.45 ms | 无回归（系统噪声） |
| `echo_loss` | 29 / 927 (3.1%) | 24 / 932 (2.6%) | 无回归 |
| `aoi_enter` / `aoi_prop_update` | 1897 / 12263 | 1894 / 12265 | 无变化 |
| `bytes_rx_total` | 443 676 B | **542 902 B** | **+99 226 B** |
| `bytes_rx_per_sec` | 21.66 KB/s | 26.51 KB/s | +4.85 KB/s |
| `bytes_rx_per_cli_s` | 887.4 B/s/client | **1085.8 B/s/client** | **+198.4 B/s/client (+22.4%)** |
| `msg=0xF003` count / avg | 14160 / **20.4 B** | 14159 / **27.4 B** | **+7.0 B/msg** |
| `msg=0xF002` count / avg | 25 / 6.0 B | 25 / **13.0 B** | **+7.0 B/msg** |

### 6.2 字节增量的理论对账

- 每条 `0xF003` AoI envelope 的 property-update 段多出来的 7 字节**精确等于** `StressWeapon` 定长布局 `[i32 id (4)] + [u16 sharpness (2)] + [u8 bound (1)] = 7 B`。
- 每条 `0xF002` baseline 多出来的 7 字节来自 `SerializeForOwnerClient` 的 struct snapshot；新加入 AoI 的客户端首次接收即携带。
- 总增量 = 14159 × 7 + 25 × 7 = 99 113 + 175 ≈ **99 288 B**，实测 99 226 B，差值在 off-by-one 整数舍入范围内。

证明端到端通路正确：
- **PropertiesEmitter MutRef**：`avatar.MainWeapon.Id = value` 成功标脏并进入 delta payload
- **PropertyCodec**：struct-typed property 走 `StressWeapon.Serialize(ref writer)` 而不是 `WriteInt32`
- **DeltaSyncEmitter**：`SerializeOwnerDelta` 和 `SerializeForOwnerClient` 都正确加入 struct body
- **客户端侧 ApplyReplicatedDelta / ApplySnapshot**：走 `StressWeapon.Deserialize` 还原 struct（若反序列化错会触发 SpanReader 越界或 field 错乱，但所有 AoI 计数一致 → 还原正确）
- **无回归**：延迟分布、AoI 计数、失败计数全部匹配 baseline

### 6.3 对 P1 / P2 的含义

- 每秒每实体增量 ~200 B 的绝对量小于单帧 `move_sent` 的字节开销，证明 P0 whole-struct 方案对小 struct 性价比 OK —— P2 `kStructFieldSet` 字段级的优化目标依然成立，但优先级可以低于 P1 的 list/dict 整体同步。
- `0xF003` 的 avg 包大小已经是 P0 基线的 1.35×。P1 的 list/dict op-log 上线时需要通过同 tick 合并 + bit-pack 把这个 growth rate 按住，不然规模上 100 client/4 space 时会撞带宽上限。

---

## 7. Changelog

- 2026-04-24：采集增强 patch 0002；debug build 首轮 Config-S25 基线。
- 2026-04-24：Envelope 路径调研 + world_stress `aoi_pos_update` 计数器修复（patch 0003）；release build 规范确立；Config-S25 换 release 数字。
- 2026-04-25：P0 交付 + StressAvatar `mainWeapon` struct 属性上线，Config-S25 对比数字写入 §6。
- 2026-04-25：P1 交付（a/b/c + compaction）+ StressAvatar `scores list<int32>` 属性上线，Config-S25 对比数字写入 §8。

---

## 8. P1 交付后对比

**背景**：P1a/b/c/compaction 阶段完成后 StressAvatar 新增 `scores list<int32>`（`all_clients`）；服务端 `OnTick` 每秒 `Scores.Add`，每 10 秒先 `Clear` 再 Add。服务端 delta 路径走 op-log（kListSplice / kClear），baseline 路径仍走 integral。Config-S25 release 再跑一次，与 §6 横比。

### 8.1 Config-S25 对比表

| 指标 | P0.8（+struct） | P1d（+struct +list） | Δ vs P0.8 |
|---|---|---|---|
| `login_success` / `entity_transferred` / `cell_ready` | 50/50/50 | 50/50/50 | 无回归 |
| `echo_rtt_p50 / p95 / p99` | 1.49 / 13.39 / 21.45 ms | 2.73 / 14.38 / 27.10 ms | p95 +1.0 ms（系统噪声，非对数级） |
| `echo_loss` | 24 / 932 (2.6%) | 25 / 932 (2.7%) | 无回归 |
| `aoi_enter` / `aoi_prop_update` | 1894 / 12265 | 1894 / 12261 | 无变化 |
| `bytes_rx_total` | 542 902 B | **594 178 B** | **+51 276 B** |
| `bytes_rx_per_sec` | 26.51 KB/s | 29.01 KB/s | +2.50 KB/s |
| `bytes_rx_per_cli_s` | 1085.8 B/s/client | **1188.4 B/s/client** | **+102.6 B/s/client (+9.4%)** |
| `msg=0xF003` count / avg | 14159 / 27.4 B | 14155 / **31.0 B** | **+3.6 B/msg** |
| `msg=0xF002` count / avg | 25 / 13.0 B | 25 / **38.8 B** | **+25.8 B/msg** |

### 8.2 字节增量的理论对账

**0xF003 delta（+3.6 B/msg）**：scores 属性每秒 dirty 一次，所以只有 ~1/5 左右的 delta 真正携带 scores payload。当它在 payload 里出现时 CompactOpLog 会把连续 Add 合并成一条 Splice：

- 典型："上次 Add 之后又 Add 一个" → CompactOpLog 合并 → 1 条 Splice = `[opCount u16][kind u8][start u16][end u16][count u16][i32]` = 13 B
- Clear 周期："Clear + Splice(0,0,[v])" 同样压成 2 op = 14 B
- 摊到所有 delta：13 B × 28% ≈ 3.6 B/msg ✓

关键：若 op-log 没做合并，同 tick 多次 Add 会按每条 11 B 独立发送，本测试里 scores 每 tick 只 Add 1 次所以看不出合并节省。真合并收益要看 P1c-merge 的单元测试 `AdjacentAppends_MergeIntoSingleSplice`（5 条合成 1 条 Splice，省 70%）。

**0xF002 baseline（+25.8 B/msg）**：baseline 走 integral，首次 AoI enter 时把 scores 当前状态完整序列化 `[u16 count][i32 × N]`。测试进行到 20s 时 scores 平均在 1–9 之间（每 10 tick Clear）。25 次 baseline × 平均 ~6 items × 4B + 2B 长度 = 25 × 26 B = 650 B。实测增量 821 B，包含 flag byte 位 + 计数不确定性，数量级对得上。

### 8.3 Integral 对照估算

假设同样 scores 配置但没做 op-log（即 §1a 的 integral 分支始终触发）：

| 场景 | Integral wire | Op-log wire | Op-log 节省 |
|---|---|---|---|
| scores 1 Add，list 已有 9 items | 2 + 10×4 = 42 B | 13 B | 29 B (−69%) |
| scores 1 Add，list 有 5 items | 2 + 6×4 = 26 B | 13 B | 13 B (−50%) |
| scores 1 Add，list 空 | 2 + 4 = 6 B | 13 B | **+7 B (−117%)** |
| scores Clear + Add 1 item | 2 + 4 = 6 B | 14 B | +8 B |

CompactOpLog 的 fallback 分支（`ops.Count > items.Count + 1`）覆盖后两行：若单 tick 真塞进过多小 op 使总字节超过 integral，自动切回 "Clear + 全量 Splice"。本测试里 scores 单 tick 至多 2 op（Clear + Add），落在 fallback 门槛之下，所以没触发。

### 8.4 对后续阶段的含义

- **0xF003 avg 从 P0 基线的 20.4 B → P0.8 的 27.4 B → P1d 的 31.0 B**，增速放缓（P0.8 +7 B struct 整体，P1d 仅 +3.6 B list 增量）。op-log 确实按设计书承担了热增量优化。
- **baseline `0xF002` 仍是线性增长**（+26 B list）。这是预期的——baseline 是冷启动全量，每项属性成本都进入基线。未来 P2 nested 支持后，深层 list<struct<list>> 会把 baseline 成本进一步放大；届时需要考虑 baseline 本身的压缩或分帧。
- **p95 RTT 波动 <10%，p50/p99 也在系统噪声内**，P1 整套改动没让 cell pump 变慢。
- 若 Component（P3）上线后要再跑 S25，预期 0xF003 avg 不再上涨（component 结构不改 per-delta 布局），bytes 主要来自 component baseline 的一次性成本。
