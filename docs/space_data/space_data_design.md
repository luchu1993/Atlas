# SpaceData

> 关联：[BigWorld SpaceData 参考](../bigworld_ref/) · [property_sync_design.md](../property_sync/property_sync_design.md) · [bsp_tree](../../src/server/cellappmgr/bsp_tree.h)

SpaceData 是 **per-space 全局 key→bytes 字典**，由 cellapp 维护，绕过 entity AoI 推送给 space 内所有 client。BigWorld 风格 `setSpaceData` 的等价物。典型用法：NPC 总数、boss 阶段、世界 buff 等不绑定到具体 entity 的全局状态。

---

## 1. 数据模型

```
Space (per cellapp)
 ├─ data_table_: std::map<u16 key_id, std::vector<u8>>
 ├─ data_initialized_: bool
 └─ bsp_tree_ (CellAppMgr 推送的 BSP)
       └─ primary leaf 所在 cellapp = owner
```

- **Key**: `u16`，全局命名空间。schema 在 `.def` 文件 `<space_data><key name="..." id="..." type="..."/>` 段声明（`entity_defs/space_data.def`），generator 生成 `SpaceDataKeys` 常量类（`SpaceDataEmitter.cs`）。
- **Value**: opaque bytes，类型化编码（little-endian）由 C# 端 typed Set/Get 包装实现（`Atlas.Space.SpaceData` server、`Atlas.Client.SpaceDataManager` client）。
- **Owner**: BSP 树最左下 leaf（`BSPTree::PrimaryCellId`）所在 cellapp。
- **Initialized flag**: owner 永远 `true`；ghost cellapp 收到 `SpaceDataSnapshot` 后置 `true`。

---

## 2. 端到端流程

```
脚本 (cellapp C#):
  Atlas.Space.SpaceData.SetInt32(spaceId, SpaceDataKeys.NpcCount, n)
    ↓ LE encode
  NativeApi.SetSpaceData → AtlasSetSpaceData (P/Invoke)
    ↓
  CellApp::SetSpaceData
    ├─ if (IsOwner)
    │    ├─ Space.Data().Set(key, value)
    │    ├─ BroadcastSpaceDataUpdate (→ all peer cellapps holding the space)
    │    └─ PushSpaceDataUpdateToWitnesses (→ each witness in space)
    └─ else
         └─ forward intercell SpaceDataUpdate → owner cellapp

Witness::SendSpaceDataUpdate → cell→client envelope [kind=6][space_id][key][vlen][bytes]
    ↓
Client (`Atlas.Client.ClientSession`):
  DispatchSpaceDataUpdate → SpaceDataManager.SetKey + KeyChanged event
    ↓
脚本 (client C#):
  spaceDataManager.GetInt32(spaceId, SpaceDataKeys.NpcCount)
```

---

## 3. Wire 协议

### 3.1 Intercell (cellapp ↔ cellapp, RUDP)

| message | id | reliability | payload |
|---|---|---|---|
| `SpaceDataUpdate` | 3130 | Reliable+Batched | `space_id, key_id, value bytes (PackedInt-prefixed)` |
| `SpaceDataDelete` | 3131 | Reliable+Batched | `space_id, key_id` |
| `SpaceDataSnapshotRequest` | 3132 | Reliable+Immediate | `space_id` |
| `SpaceDataSnapshot` | 3133 | Reliable+Immediate | `space_id, count, [(key_id, value)*]` |

### 3.2 Envelope (cell → client)

| kind | u32 id-field | payload |
|---|---|---|
| `kSpaceDataInit` (5) | space_id | `[u32 count][(u16 key, u32 vlen, vbytes)*]` |
| `kSpaceDataUpdate` (6) | space_id | `[u16 key, u32 vlen, vbytes]` |
| `kSpaceDataDelete` (7) | space_id | `[u16 key]` |

Envelope 走 reliable channel (`kClientReliableDeltaMessageId 0xF003`)；wire 上 u32 id-field 在 kSpaceData* 上语义为 space_id（其它 kind 为 entity_id），client dispatcher 按 kind 分流。

---

## 4. Owner 模型

BSP 树 split 时**保留原 leaf 在左侧**（`bsp_tree.cc::Split`），所以最左下 leaf 永远是原始分配的 cell。owner = 该 leaf 的 cellapp。

- 初始：`CellAppMgr::PickHostForNewSpace` 选负载最低 cellapp（`cellappmgr.cc::OnCreateSpaceRequest`）
- BSP balance：split line 移动不改 leaf 归属，所以 owner 不变
- Owner cellapp 死亡：`CellAppMgr::OnCellAppDeath` 用 `PickAlternateHostInSpace` 优先选**已持该 space 任意 leaf 的存活 cellapp**——新 owner 的本地 ghost SpaceData 副本即 source of truth。fallback 到 `PickAlternateHost`。新 BSP 广播后，新 owner 在 `Space::SetBspTree` 中 `IsOwner()` 返回 true 并 `MarkDataInitialized`。

---

## 5. 新 cellapp 加入握手

`CellApp::OnUpdateGeometry` 处理 BSP 更新后，若 `!IsDataInitialized() && !IsOwner()`：
```cpp
if (auto* owner_ch = FindSpaceOwnerChannel(*space)) {
  owner_ch->SendMessage(SpaceDataSnapshotRequest{space_id});
}
```
幂等——下次 BSP 更新若 channel 还没 ready 会再试。Owner 回 `SpaceDataSnapshot`（含全量 entries）；接收端 `Clear + Set*` + `MarkDataInitialized` + `PushSpaceDataInitToWitnesses`。

---

## 6. 持久化（设计共识：不在引擎层）

**SpaceData 本身不持久化**——BigWorld Server Programming Guide 明确："Space data is not persistent. If persistence is required, store it on the space entity's properties." Atlas 沿用此哲学：

- 引擎层 SpaceData 是 runtime cache，cellapp 重启即丢
- 持久化通过 **space-owner entity** 的 `persistent="true"` 属性（复用现有 entity persistence 通道）
- Reload 时脚本在 `OnSpaceInit(isReload=true)` 中把 persistent 属性写回 SpaceData

参见 [`CellSpaceEntity.cs`](../../src/csharp/Atlas.Runtime/Space/CellSpaceEntity.cs) + [`SpaceOwnerRegistry.cs`](../../src/csharp/Atlas.Runtime/Space/SpaceOwnerRegistry.cs)。

---

## 7. C# API

**Server**（`Atlas.Runtime/Space/SpaceData.cs`）：
```csharp
SpaceData.SetInt32(spaceId, keyId, value);
SpaceData.SetString(spaceId, keyId, "hello");
SpaceData.Remove(spaceId, keyId);
```
8 个 typed Set 方法对应 `int32/int64/uint32/uint64/float/double/bool/string`，wire 编码 little-endian。Generator 生成的 `SpaceDataKeys` 提供编译期常量 + `NameOf`/`KindOf` 反查。

**Client**（`Atlas.Client/SpaceDataManager.cs`）：
```csharp
session.SpaceDataManager.KeyChanged += (sp, k, v) => ...;
var count = session.SpaceDataManager.GetInt32(spaceId, SpaceDataKeys.NpcCount);
```
事件驱动（`KeyChanged / KeyRemoved / Initialized`）+ typed getters with fallback。
`ClientCallbacks.SpaceDataManager` 仍指向 `DefaultSession`，供旧宿主和简单
工具使用；Unity / Desktop host 应优先持有自己的 `ClientSession`。

**Space-owner entity** (`CellSpaceEntity`)：派生类 OnInit/OnDestroy 由基类 sealed override 接管 → 自动注册 `SpaceOwnerRegistry`；脚本覆盖 `OnSpaceInit/OnSpaceDestroy`。`SpaceOwnerRegistry.Find(spaceId)` 单线程 lookup（cellapp 单线程脚本）。

**Entity-level timer**（`ServerEntity.StartTimer / CancelTimer`）：refill 循环用 per-entity TimerController（C++ 端 `src/lib/space/timer_controller.h`），状态随 entity offload 一起迁移（`controller_codec.cc` 序列化 interval / repeat / accumulated / fire_count）。Action delegate 是 managed 状态，offload / hot-reload 时丢失，脚本应在 `OnInit(isReload=true)` 中 `StartTimer` 重建。**避免**用 process-level `AtlasLoop.Current.RegisterTimer` —— 后者在 cellapp handoff 时不迁移状态。

---

## 8. 性能

| 路径 | 复杂度 | 备注 |
|---|---|---|
| `Space::IsOwner()` | O(1) | 通过 `BSPTree::PrimaryCellId()` cache 字段 + `local_cells_.count` 哈希 |
| `BSPTree::PrimaryCellId()` | O(1) | 缓存在 `primary_cell_id_`；invariant: Split 保留原 leaf 在左侧，所以 InitSingleCell / Deserialize 之外永不变化 |
| `SpaceData::Set` (cellapp local) | O(value bytes) | `std::map` 查找 O(log K) + `std::equal` size-check 短路 |
| `BroadcastSpaceDataUpdate` | O(P peer cellapps) | 遍历 BSP leaves 去重；典型 P < 10 |
| `PushSpaceDataUpdateToWitnesses` | O(N entities) | 遍历 space 所有 entity 找 witness；profile zone `CellApp::PushSpaceDataUpdate` 已挂，scale 时可优化为 witness-by-space 索引 |
| `OnSpaceDataUpdate` (handler) | O(value bytes) + 上述两项 | 已写过滤（`Data().Set` 等值 noop） |

---

## 9. 与 BigWorld / KBE 的对比

| 项 | BigWorld | KBE master | Atlas |
|---|---|---|---|
| key 类型 | string | string | typed u16 (def schema) |
| value 类型 | python pickle | python pickle | typed bytes (LE 编码) |
| owner 模型 | CellAppMgr authority | 每 cellapp 独立写本地副本（**脑裂**） | BSP primary cell cellapp (owner-cell 直连广播) |
| 持久化 | space entity properties | space entity properties (脚本约定) | space entity properties (`CellSpaceEntity`) |
| Hot-standby cellappmgr | ✓ (14.4) | ✗ | ✗ (Atlas cellappmgr 是独立 epic) |
| 跨 cellapp 一致性 | cellappmgr 中转 (total-order) | **未实现** | owner-cell 直连 (RUDP FIFO) |

Atlas 选 BigWorld 哲学 + owner-cell 直连优化（省一跳 cellappmgr 中转，热路径 FIFO 由 RUDP channel 保证）。
