# Phase 10 前置 PR 清单

> 本文是 [`phase10_cellapp.md`](./phase10_cellapp.md) 的 **前置依赖** 文档。
> 这组 4 份 PR（PR-A / PR-B / PR-C / PR-D）**不属于 Phase 10 本体**，
> 但 Phase 10 的 Step 10.5b / 10.7 / 10.8 / 10.9 硬依赖它们。
>
> 建立时间: 2026-04-18。来源于原 `phase10_cellapp.md` §7.1。
>
> **状态（2026-04-22）**: 四份前置 PR **均已合入**；本文保留作为 Phase 10 启动前的设计档，§3–§6 的实施细节可作为对照参考。

---

## 0. 前置 PR 总览

| PR | 名称 | 主要改动 | 状态 |
|----|------|---------|------|
| **PR-A** | BaseApp `ClientCellRpc` struct + handler 骨架 | `baseapp_messages.h:800-851` 新增 struct；`baseapp.cc` 新增 handler；客户端 SDK 发送 | ✅ 已落地 |
| **PR-B** | DEF_GENERATOR Step 5 C++ 部分 | `entity_def_registry` 新增 `RpcDescriptor.direction/exposed` + `ExposedScope` 查询 | ✅ 已落地 |
| **PR-C** | Script Phase 4 — C# 序号 + 受众过滤 delta | `Atlas.Runtime` `ReplicationFrameHandle` 产出 owner/other snapshot + delta + `event_seq` + `volatile_seq` | ✅ 已落地 |
| **PR-D** | BaseApp 有序可靠 delta 通道（路径分离策略） | `src/server/baseapp/delta_forwarder.h/.cc` + 单元测试 | ✅ 已落地 |

---

## 1. 与 Phase 10 本体的关系

### 1.1 为什么抽到前置

这 4 项都是"改变 Phase 10 开工前提"的变更。把它们塞进 Phase 10 本体会导致：

- Step 10.5b 依赖 PR-C 才能有 `event_seq` 可用，写进同一批 PR 会互相阻塞
- Step 10.9 依赖 PR-A (struct) + PR-B (direction) 才能做校验接线；合并会让 reviewer 难以拆分 RPC 校验逻辑与消息定义
- PR-D 本质是"不改 `DeltaForwarder`"的负向决策，落在文档与单测里即可，放在 Phase 10 本体反而混淆路径分离原则

抽出后 Phase 10 Step 10.1–10.10 只需一条判据：**"4 份前置合入后才能启动"**。

### 1.2 与 `DEF_GENERATOR_DESIGN.md` Step 5 的映射

PR-B 即 DEF_GENERATOR_DESIGN Step 5 的 C++ 部分。两份文档必须保持一致：

- 本文对 `RpcDescriptor` 的签名变更 → 必须同步回 `DEF_GENERATOR_DESIGN.md` Step 5
- `DEF_GENERATOR_DESIGN.md` Step 5 若进一步细化字段（例如新增 `args_count`），本文需跟进
- 实现 PR 合入时应在两份文档同时打勾或引用 PR 号

---

## 2. 当前代码基线核查（2026-04-18 → 2026-04-22）

> 2026-04-18 快照：前置 PR 尚未合入，四项空缺；以下表格是当时核查的证据。
> 2026-04-22 现状：四份 PR 均已合入，表内"❌"项已转为"✅"（`ClientCellRpc` 在 `baseapp_messages.h:800-851`、`.def` 解析 + `RpcDescriptor.direction/exposed` 在 `entity_def_registry` 中、C# `event_seq/volatile_seq` 在 `ReplicationFrameHandle`）。保留此表作为历史对照，不再作为前置校验清单。

| 检查项 | 2026-04-18 | 2026-04-22 |
|---|---|---|
| `ClientCellRpc` (2023) 存在？ | ❌ 仅枚举值 | ✅ struct + handler |
| `ReplicatedDeltaFromCell` (2015) Reliable？ | ❌ Unreliable | ✅ 新增 reliable 变体 `ReplicatedReliableDeltaFromCell` (2017) |
| `.def` 解析器存在？ | ❌ 仅 `FromJsonFile` | ✅ 落地 |
| `RpcDescriptor.direction / exposed` 字段？ | ❌ 未定义 | ✅ `ExposedScope` 查询可用 |
| C# `event_seq / volatile_seq`？ | ❌ 不存在 | ✅ `ReplicationFrameHandle` / `ServerEntity` 就位 |

---

## 3. PR-A: BaseApp `ClientCellRpc` struct + handler 骨架

### 3.1 改动文件

```
src/server/baseapp/baseapp_messages.h   (新增 struct)
src/server/baseapp/baseapp.h            (新增 handler 声明)
src/server/baseapp/baseapp.cc           (新增 handler 实现 + 消息分发)
src/client/client_native_provider.h     (SendCellRpc 存在；.cc 实现核查)
src/client/client_native_provider.cc    (SendCellRpc 改为真实发送)
tests/unit/test_baseapp_messages.cc     (ClientCellRpc 序列化/反序列化)
tests/unit/test_baseapp_client_rpc.cc   (handler 连通性)
```

### 3.2 struct 定义

```cpp
// baseapp_messages.h，紧跟 ClientBaseRpc (2022) 之后
namespace atlas::baseapp {

// 客户端 → BaseApp：请求调用目标实体 Cell 上的 exposed 方法
// 由 BaseApp 校验后，构造 cellapp::ClientCellRpcForward 转发到 CellApp
struct ClientCellRpc {
  EntityID target_entity_id;
  uint32_t rpc_id;
  std::vector<std::byte> payload;

  static constexpr MessageID kId = static_cast<MessageID>(2023);
  static constexpr MessageReliability kReliability = MessageReliability::kReliable;

  void Serialize(SpanWriter& w) const;
  static auto Deserialize(SpanReader& r) -> ClientCellRpc;
};

}  // namespace atlas::baseapp
```

### 3.3 handler 骨架

```cpp
// baseapp.h
void OnClientCellRpc(Channel& ch, const baseapp::ClientCellRpc& msg);

// baseapp.cc（PR-A 只放骨架，完整校验在 Step 10.9 接线）
void BaseApp::OnClientCellRpc(Channel& /*ch*/, const baseapp::ClientCellRpc& msg) {
  auto* proxy = FindProxyByChannel(/* ch */);
  if (!proxy) {
    ATLAS_LOG_WARNING("ClientCellRpc: no proxy for channel, dropping rpc_id={:#x}",
                      msg.rpc_id);
    return;
  }

  // TODO(Step 10.9 / PR-B): 接入 FindRpc().direction == 0x02 + is_exposed + 跨实体校验
  ATLAS_LOG_WARNING("ClientCellRpc accepted without validation (PR-B pending): "
                    "proxy={} target={} rpc_id={:#x} payload_bytes={}",
                    proxy->entity_id(), msg.target_entity_id,
                    msg.rpc_id, msg.payload.size());

  // TODO(Step 10.9): 构造 cellapp::ClientCellRpcForward 转发到 CellApp
}
```

### 3.4 消息路由注册

在 `baseapp.cc` 的消息分发表中追加：

```cpp
case 2023:  // ClientCellRpc
  OnClientCellRpc(*ch, baseapp::ClientCellRpc::Deserialize(reader));
  break;
```

### 3.5 客户端 SDK 改造

`src/client/client_native_provider.cc::SendCellRpc()` 需从"仅打日志的 stub"改为：

```cpp
void ClientNativeProvider::SendCellRpc(uint32_t target_entity_id, uint32_t rpc_id,
                                       const std::byte* payload, int32_t len) {
  if (!client_.has_value() || !client_->IsConnected()) return;

  baseapp::ClientCellRpc msg{};
  msg.target_entity_id = target_entity_id;
  msg.rpc_id = rpc_id;
  msg.payload.assign(payload, payload + len);

  SpanWriter w;
  msg.Serialize(w);
  client_->Send(baseapp::ClientCellRpc::kId, w.Span());
}
```

### 3.6 验收标准

- [ ] `ClientCellRpc::Serialize` → `Deserialize` roundtrip 字节一致
- [ ] BaseApp 收到客户端 `ClientCellRpc` 后打 WARN（因校验尚未接线）
- [ ] 客户端 SDK `SendCellRpc()` 真实投递到 channel（不是 stub）
- [ ] handler 在 `proxy == nullptr` 时安全丢弃
- [ ] 消息 ID 2023 出现在 `baseapp.cc` 的 dispatch 表中

---

## 4. PR-B: DEF_GENERATOR Step 5 C++ 部分

### 4.1 改动文件

```
src/lib/entitydef/entity_def_registry.h       (RpcDescriptor 扩展字段)
src/lib/entitydef/entity_def_registry.cc      (FindRpc / GetExposedScope 基于 .def 数据)
src/lib/entitydef/def_file_loader.h / .cc     (新增 — .def 解析器)
src/csharp/Atlas.Generators.Def/DefGenerator.cs (同步 direction metadata)
tests/unit/test_entity_def_registry.cc
tests/unit/test_def_file_loader.cc            (新增)
```

### 4.2 RpcDescriptor 签名

```cpp
// entity_def_registry.h
enum class ExposedScope : uint8_t { None = 0, OwnClient = 1, AllClients = 2 };

struct ArgDescriptor {
  std::string name;
  std::string type_name;
  // 序列化/反序列化元信息由生成器注入，Phase 10 不必落地完整类型系统
};

struct RpcDescriptor {
  uint32_t rpc_id;
  std::string method_name;
  uint8_t direction;           // 0x01=Client, 0x02=Cell, 0x03=Base（对应实体方法的归属）
  ExposedScope exposed;        // None / OwnClient / AllClients（仅 Cell/Base 方法有意义）
  std::vector<ArgDescriptor> args;
};

class EntityDefRegistry {
 public:
  [[nodiscard]] auto FindRpc(uint32_t rpc_id) const -> const RpcDescriptor*;
  [[nodiscard]] auto GetExposedScope(uint32_t rpc_id) const -> ExposedScope;
  // ...
};
```

### 4.3 `.def` 解析器

目标：接受 BigWorld 风格 XML（或 Atlas 等价 JSON/TOML，见 DEF_GENERATOR_DESIGN）并填充 `RpcDescriptor`。

- 解析 `<Cell>/<Base>/<Client>` 段下的方法节点
- 每个方法抽出：名称、参数列表、`<Exposed/>` 标签（OwnClient / AllClients / 缺省=None）
- `direction` 由所在段归属决定（Cell → 0x02，Base → 0x03，Client → 0x01）
- `rpc_id` 由 Generator 计算（同 C# 侧 hash 规则），加载 `.def` 时采用相同 hash

### 4.4 C# 生成器同步

`Atlas.Generators.Def/DefGenerator.cs` 已产出 `ExposedScope` 元数据；需追加 `Direction`（`byte` 常量），写入生成的 registry 初始化代码，保证 C++ 与 C# 对同一 `rpc_id` 看到相同 direction。

### 4.5 验收标准

- [ ] `.def` 文件（单测 fixture）加载后：每条 RPC 都有正确的 `rpc_id / direction / exposed`
- [ ] `GetExposedScope(非 exposed 方法)` 返回 `ExposedScope::None`
- [ ] Cell 方向方法 `FindRpc().direction == 0x02`
- [ ] Base 方向方法 `FindRpc().direction == 0x03`
- [ ] Client 方向方法 `FindRpc().direction == 0x01`
- [ ] 同一 entity class 的 C# 生成器 metadata 与 C++ `.def` 加载结果一致（跨语言哈希测试）

---

## 5. PR-C: Script Phase 4 — C# 序号 + 受众过滤 delta

### 5.1 改动文件

```
src/csharp/Atlas.Generators.Def/Emitters/DeltaSyncEmitter.cs   (大改)
src/csharp/Atlas.Generators.Def/DefModel.cs                    (可能补字段)
src/csharp/Atlas.Runtime/ReplicationFrameHandle.cs             (新增)
src/csharp/Atlas.Runtime/CellEntityBase.cs                     (新增 BuildAndConsumeReplicationFrame)
src/csharp/Atlas.Runtime.Tests/ReplicationFrameTests.cs        (新增)
```

### 5.2 现状对比

`DeltaSyncEmitter.cs` 当前产出：

| 方法 | 语义 | 当前状态 |
|------|------|---------|
| `SerializeReplicatedDelta(writer)` | 按 `_dirtyFlags` 写全部可复制 dirty 属性 | ✓ 存在，但无受众切分、无序号 |
| `SerializeForOwnerClient(writer)` | Owner 视角整体 **快照**（审计 `IsOwnerVisible`） | ✓ 存在 |
| `SerializeForOtherClients(writer)` | Other 视角整体 **快照**（审计 `IsOtherVisible`） | ✓ 存在 |

Phase 10 需要新增：按受众切分的 **delta**（不是 snapshot）+ 序号。

### 5.3 新增 API 签名

```csharp
// Atlas.Runtime/ReplicationFrameHandle.cs
public readonly struct ReplicationFrameHandle
{
    public ulong EventSeq { get; }          // 属性事件序号，累积，按 1 递增（0 = 本帧无属性变更）
    public ulong VolatileSeq { get; }       // Volatile（位置/朝向）序号，latest-wins（0 = 本帧无 volatile 变更）
    public ReadOnlySpan<byte> OwnerSnapshot { get; }   // 复用 SerializeForOwnerClient
    public ReadOnlySpan<byte> OtherSnapshot { get; }   // 复用 SerializeForOtherClients
    public ReadOnlySpan<byte> OwnerDelta { get; }      // 新增：audience = {OwnClient, AllClients, CellPublicAndOwn, BaseAndClient}
    public ReadOnlySpan<byte> OtherDelta { get; }      // 新增：audience = {AllClients, OtherClients}
}

// Atlas.Runtime/CellEntityBase.cs
public partial class CellEntityBase
{
    internal ReplicationFrameHandle BuildAndConsumeReplicationFrame();
    // Generator-emitted partial implementation per concrete entity class
}
```

### 5.4 生成器改动要点

`DeltaSyncEmitter.cs` 需追加：

1. **per-type audience mask 常量**（两份 `ulong`，对应 `_ownerVisibleMask` / `_otherVisibleMask`）：
   ```csharp
   private const ulong _ownerVisibleMask = /* bit-or of props where IsOwnerVisible(scope) == true */;
   private const ulong _otherVisibleMask = /* bit-or of props where IsOtherVisible(scope) == true */;
   ```
2. **`SerializeOwnerDelta(writer)` / `SerializeOtherDelta(writer)` 两个新方法**，按 `_dirtyFlags & mask` 迭代写；dirty 位完全不相交则方法产出 0 字节
3. **`BuildAndConsumeReplicationFrame()` 的 partial 实现**：
   - 若 `_dirtyFlags & (_ownerVisibleMask | _otherVisibleMask) != 0`，增 `_eventSeq`，调用两个新方法到两个独立 `SpanWriter`，构造 OwnerSnapshot / OtherSnapshot（全量）
   - 若位置/朝向脏位被置，增 `_volatileSeq`
   - 清零 `_dirtyFlags`（语义：frame 被"消费"）
   - 返回 `ReplicationFrameHandle`

### 5.5 关键规则（与 `phase10_cellapp.md` §3.3/§3.4 对齐）

- **`EventSeq` 单调递增**：只要本 tick 任一 client-visible 属性 dirty，`EventSeq++` 一次
- **`VolatileSeq` 独立于 EventSeq**：位置/朝向独立序号，不占 `EventSeq`
- **受众切分必须与 `DeltaSyncEmitter.IsOwnerVisible / IsOtherVisible` 一致**：
  - `OwnerDelta` audience = `{OwnClient, AllClients, CellPublicAndOwn, BaseAndClient}`
  - `OtherDelta` audience = `{AllClients, OtherClients}`
  - ⚠ `CellPublicAndOwn` **仅** 在 OwnerDelta 出现，不得写入 OtherDelta
- **Snapshot vs Delta 一帧并存**：Owner/OtherSnapshot 复用现有序列化；Owner/OtherDelta 为新增；Witness 侧 REFRESH/ENTER 用 snapshot，普通追帧用 delta

### 5.6 验收标准

- [ ] 修改 `OwnClient` 属性 → OwnerDelta 非空，OtherDelta 为空
- [ ] 修改 `OtherClients` 属性 → OwnerDelta 为空，OtherDelta 非空
- [ ] 修改 `AllClients` 属性 → 两路 delta 均非空
- [ ] 修改 `CellPublicAndOwn` 属性 → 仅 OwnerDelta 非空（`OtherDelta == ReadOnlySpan<byte>.Empty`）
- [ ] 修改 `CellPrivate / CellPublic / Base` 属性 → 两路 delta 均空（非 client-visible）
- [ ] 无脏位 → `EventSeq` 不变（frame 不发布或标记 event_seq=0）
- [ ] 仅修改位置 → `EventSeq` 不变，`VolatileSeq++`
- [ ] 连续 10 次属性变更 → `EventSeq` 严格递增 1..10
- [ ] audience mask 常量与 `IsOwnerVisible / IsOtherVisible` 结果一致（反射单测）

---

## 6. PR-D: BaseApp 有序可靠 delta 通道

### 6.1 问题陈述

`DeltaForwarder::Enqueue()` 对同实体 **覆盖旧 delta**（`delta_forwarder.h:25-28`："When the same entity receives a new delta while a previous one is still queued, the old entry is replaced — the client only needs the latest state"）。

这对 Volatile 位置正确（旧帧无意义），但会**吃掉属性 delta 中间帧**，破坏 `event_seq` 有序性——导致客户端状态永久偏离。

### 6.2 Phase 10 方案：路径分离（不改现有 DeltaForwarder）

```
┌────────────────────────────────────────────────────────────────────┐
│                         BaseApp 下行路径                            │
├────────────────────────────────────────────────────────────────────┤
│                                                                    │
│  Volatile 位置更新                                                  │
│    ReplicatedDeltaFromCell (2015, Unreliable)                      │
│      → OnReplicatedDeltaFromCell()                                 │
│      → client_delta_forwarders_[addr].Enqueue()   ← latest-wins    │
│      → FlushClientDeltas()                                          │
│      → 客户端 channel with MessageID 0xF001                         │
│                                                                    │
│  属性 delta（event_seq 有序）                                        │
│    SelfRpcFromCell (2014, Reliable)                                │
│      → OnSelfRpcFromCell()                                         │
│      → 直达客户端 channel      ← 不经 DeltaForwarder                │
│      → payload 内封 CellAoIEnvelope{kind=EntityPropertyUpdate}     │
│                                                                    │
│  广播属性 delta（AllClients, Phase 10 本体）                         │
│    由 CellApp 按观察者逐个展开，每个观察者一条 SelfRpcFromCell        │
│    ← 不在 BaseApp 侧做 fan-out（BroadcastRpcFromCell 当前不支持）    │
│                                                                    │
└────────────────────────────────────────────────────────────────────┘
```

### 6.3 改动文件

PR-D 本身 **不写新业务代码**，只落文档与测试：

```
src/server/baseapp/delta_forwarder.h           (注释补"仅服务 Volatile 路径")
src/server/baseapp/delta_forwarder.cc          (Enqueue 入口 DCHECK，见 6.4)
src/server/baseapp/baseapp.cc                  (OnSelfRpcFromCell / OnReplicatedDeltaFromCell 注释交叉引用)
tests/unit/test_delta_forwarder.cc             (新增反向断言测试)
tests/unit/test_baseapp_self_rpc_path.cc       (新增路径断言测试)
```

### 6.4 建议的 DCHECK

为了把"属性 delta 禁止入队 DeltaForwarder"的约束变为运行期断言，在 `DeltaForwarder::Enqueue` 入口增加可选 payload 类型标签的 DCHECK（仅 Debug 构建）：

```cpp
// delta_forwarder.cc
void DeltaForwarder::Enqueue(EntityID entity_id, std::span<const std::byte> delta) {
  // Volatile payload 必须以 CellAoIEnvelopeKind::EntityPositionUpdate 开头。
  // Release 构建下无额外开销；Debug 构建提前暴露路径错用。
  ATLAS_DCHECK(!delta.empty() &&
               static_cast<uint8_t>(delta[0]) ==
                 static_cast<uint8_t>(CellAoIEnvelopeKind::EntityPositionUpdate))
      << "DeltaForwarder only serves Volatile path. "
      << "Property delta must be routed via SelfRpcFromCell, not enqueued here.";
  // ...existing logic...
}
```

（如果 Phase 10 决定不在 DeltaForwarder 入口做 kind 识别，可改为："入口仅记录 queued-by-path 标签，统计两路 payload 的来源"。）

### 6.5 验收标准

- [ ] `delta_forwarder.h` 注释新增"Volatile-only / property delta must not be enqueued"
- [ ] 单测：连续 10 条属性 delta（`event_seq=1..10`，全部经 `SelfRpcFromCell`）全部按顺序到达客户端
- [ ] 单测：连续 10 条 Volatile 位置更新（同实体，`volatile_seq=1..10`，全部经 `ReplicatedDeltaFromCell`）→ 客户端只收到最后 1 条（latest-wins 正确）
- [ ] 单测：`OnSelfRpcFromCell()` 携带 `CellAoIEnvelope{kind=EntityPropertyUpdate}` → 直达客户端 channel，`client_delta_forwarders_` 的 `queue_depth()` 不变
- [ ] DCHECK（如启用）：在 Debug 构建下误入队属性 delta 会立即 abort 并打印错误

---

## 7. 合入策略

### 7.1 分支策略

每份 PR 独立分支，建议命名：

```
phase10/prereq-a-client-cell-rpc
phase10/prereq-b-def-parser
phase10/prereq-c-replication-frame
phase10/prereq-d-delta-forwarder-docs
```

### 7.2 合入顺序

**无强制顺序**（互无依赖），但推荐：

1. **PR-D 最先合入**（最小改动，锁定下行路径决策，让 PR-A / PR-C 的下游路径有明确契约）
2. **PR-A + PR-B 并行**（尺寸均衡、互无依赖）
3. **PR-C 最后合入**（规模最大、测试面最广；等 PR-D 的路径决策稳定后开始实现，避免重写）

### 7.3 合入后检查清单（Phase 10 Step 10.1 启动前）

- [ ] `git log --all` 可查到 PR-A/B/C/D 的合并 commit
- [ ] `cmake --build build/debug --config Debug` 成功
- [ ] `ctest --build-config Debug --label-regex unit` 全部通过
- [ ] 本文 §3.6 / §4.5 / §5.6 / §6.5 的 **所有验收勾选项** 均打勾
- [ ] `phase10_cellapp.md` §7.0 表与 §验收标准"前置"段也全部打勾

---

## 8. 前置 PR 依赖图

```
PR-D (delta 通道策略，负向决策+测试) ─┐
                                      │
PR-A (ClientCellRpc struct + SDK)    ─┼─→ Phase 10 Step 10.8 / 10.9
PR-B (.def 解析 + RpcDescriptor)     ─┼─→ Phase 10 Step 10.8 / 10.9
                                      │
PR-C (C# seq + 受众 delta)           ─┴─→ Phase 10 Step 10.5b / 10.7
```

PR-A / PR-B / PR-C / PR-D **可完全并行**；Phase 10 Step 10.5b 需等 PR-C + PR-D 合入，Step 10.8 / 10.9 需等 PR-A + PR-B 合入。

---

## 9. 相关文档

- [`phase10_cellapp.md`](./phase10_cellapp.md) — Phase 10 主设计文档（本文是其前置）
- [`../generator/DEF_GENERATOR_DESIGN.md`](../generator/DEF_GENERATOR_DESIGN.md) Step 5 — PR-B 的上游设计源
- [`../bigworld_ref/BIGWORLD_RPC_REFERENCE.md`](../bigworld_ref/BIGWORLD_RPC_REFERENCE.md) Section 3.1 / 3.3 — PR-A 的 RPC 语义参考
- [`phase09_login_flow.md`](./phase09_login_flow.md) — Phase 9 已落地（LoginApp + BaseAppMgr 基础设施）
