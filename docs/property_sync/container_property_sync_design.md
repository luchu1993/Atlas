# 容器属性同步

> 关联：[property_sync_design.md](./property_sync_design.md) · [component_design.md](./component_design.md) · [BigWorld EventHistory 参考](../bigworld_ref/BIGWORLD_EVENT_HISTORY_REFERENCE.md)

`.def` 在标量基础上支持 **struct / list[T] / dict[K,V]** 与任意层级嵌套。容器同步走 op-log（精确元素级）而非整体重传，对带宽敏感的列表 / 字典属性效果最显著。

---

## 1. 类型表达式

```
<type-expr> ::= <base-type>                             // int32, string, vector3, ...
              | <struct-name>                           // <types> 中声明的 struct 或 alias
              | "list[" <type-expr> "]"
              | "dict[" <key-type> "," <type-expr> "]"
<key-type>  ::= string | int32 | uint32 | int64 | uint64
```

- 嵌套深度 ≤ 8（DEF010）
- dict key 必须标量（DEF011）
- 容器 `max_size`（默认 4096）— 越界丢 op 并报错日志
- 解析在 `DefTypeExprParser`，递归生成 `DataTypeRefModel`

---

## 2. `<types>` 段：struct 与 alias

```xml
<entity name="Avatar">
  <types>
    <struct name="ItemStack">
      <field name="id"    type="int32"  />
      <field name="count" type="uint16" />
    </struct>

    <struct name="BuffInst" sync="field">
      <field name="kind"   type="uint32" />
      <field name="stack"  type="uint8"  />
      <field name="end"    type="float"  />
    </struct>

    <alias name="Inventory" type="list[ItemStack]" />
  </types>

  <properties>
    <property name="bag" type="Inventory" scope="own_client" max_size="128" />
  </properties>
</entity>
```

- struct 字段不允许 `scope` / `persistent` / `reliable` —— 继承所属属性
- struct 可循环引用 → 拓扑检测，DEF013 注册期硬失败
- struct 名跨实体唯一（`DefLinker` 全局表）；与 alias 同名即冲突（DEF016）
- alias 仅做类型替换，链长 ≤ 某阈值（DEF017）

跨实体共享：`<types>` 段的 struct 全部进 `DefLinker` 的全局 `StructsByName` 表，emitter 据此发 `Atlas.Def.<Name>` partial struct + `<Name>.Struct.g.cs`。

### 2.1 Struct 同步策略：`sync="auto|whole|field"`

| 模式 | wire | 适合 |
|---|---|---|
| `whole` | 整 struct 一次序列化 | 字段少 / 总字节小 / 含变长字段 |
| `field` | 字段级 op-log（`OpKind.StructFieldSet`） | 字段多且大、热改动集中在少数字段 |
| `auto` | emitter 启发式选 + DEF014 Info 透传 | 默认 |

`StructEmitter.DecideSyncMode` 启发式（短路）：

1. 含变长字段（`string` / `bytes` / `list` / `dict` / 嵌套 struct） → **whole**
2. 定长 ≤ 8B → **whole**
3. 字段数 ≤ 4 → **whole**
4. ≥ 32B 且 ≥ 8 字段 → **field**
5. 其余 → **whole**（保守默认）

`field` 模式生成 `<Struct>ItemAt` 类（class，避免 CS1612），让 `list[Struct]` 元素的字段写入也走 op-log。

---

## 3. C# 运行时

### 3.1 Struct property —— `MutRef` 拦截 setter

```csharp
public partial class Avatar
{
    private ItemStack _mainWeapon;
    private MainWeaponMutRef? __mainWeaponMutRef;
    public  MainWeaponMutRef MainWeapon => __mainWeaponMutRef ??= new(this);
    public  ItemStack        MainWeaponValue => _mainWeapon;

    public sealed class MainWeaponMutRef
    {
        private readonly Avatar _owner;
        internal MainWeaponMutRef(Avatar o) { _owner = o; }
        public ushort Count
        {
            get => _owner._mainWeapon.Count;
            set
            {
                if (_owner._mainWeapon.Count == value) return;
                _owner._mainWeapon.Count = value;
                _owner._dirtyFlags |= ReplicatedDirtyFlags.MainWeapon;
            }
        }
        public static implicit operator ItemStack(MainWeaponMutRef r) => r._owner._mainWeapon;
    }
}
```

为什么是 `sealed class` 而不是 `readonly struct`：C# 编译器对**按值返回**的 struct 字段赋值统一拒（CS1612），不区分 setter 是否触碰 `this`。class 需要 lazy-cache 一份实例（首访 ~24 B、后续 0 alloc）；5000 entity × ~3 个 struct 属性 ≈ 360 KB 长期常驻堆，对 Mono / IL2CPP 可忽略。`list[struct]` 元素 mutator (`ItemAt`) 同理。

脚本：
```csharp
avatar.MainWeapon.Count = 5;     // setter 拦截 + 标脏
ItemStack snap = avatar.MainWeapon;   // 隐式转值
```

### 3.2 Container property —— `ObservableList<T>` / `ObservableDict<K,V>`

`Atlas.Shared/Observable/ObservableList.cs` / `ObservableDict.cs` 是泛型实现（不再走 generator-per-type 的非泛型方案）。每个容器懒构造，构造时把 `MarkDirty` 闭包传入：

```csharp
private ObservableList<int>? __scoresList;
public ObservableList<int> Scores =>
    __scoresList ??= new(() => _dirtyFlags |= ReplicatedDirtyFlags.Scores);
```

容器内部维护 op buffer，setter / Add / RemoveAt / Clear 各自记录 `OpKind` + path，帧末由 `BuildAndConsumeReplicationFrame` 串接进 audience delta 的 ContainerOp 段。

`list[struct (sync=field)]` 元素的字段写入：

```csharp
avatar.PartyAt(0).Hp = 5;      // PartyAt 返回 <Struct>ItemAt class，记 kStructFieldSet op
```

---

## 4. 线上协议

### 4.1 通道

容器 op-log **只走 reliable**（`kReplicatedReliableDeltaFromCell` 2017 → `0xF003`）。原因：op-log 的语义是"基于上一个状态做精确变换"，丢一帧即客户端状态发散；DeltaForwarder 的 latest-wins 会吞中间帧，禁用。

`reliable="true"` 标量属性同帧搭便车（同一 reliable envelope 内 sectionMask bit0 = scalar、bit1 = container）。

### 4.2 Audience delta 字节布局

```
[u8 sectionMask]                        bit0 = scalar dirty / bit1 = container dirty / bit2 = component dirty
[if bit0] [flags : u8/u16/u32/u64] [scalar values...]
[if bit1] [flags : u8/u16/u32/u64] [container ops...]
[if bit2] [u8 activeSlots] [for each slot: u8 slotIdx + per-component delta...]
```

flags 整型按属性数量自动选型。Container ops 每条按 path 编码：

```
[u8 opKind]
[path segments...]                       内部线 packed_int / 外部线 bit-pack（未启用）
[op-specific payload]
```

### 4.3 OpKind 列表（`Atlas.Observable.OpKind`）

| 值 | 名 | 用途 |
|---|---|---|
| 0 | `Set` | 标量 / struct-whole 在某 leaf 的整值替换 |
| 1 | `ListSplice` | `list[T]`：删除 [start, end) 后插入 N 个值 |
| 2 | `DictSet` | `dict[K,V]`：插入或覆盖 |
| 3 | `DictErase` | `dict[K,V]`：删除 key |
| 4 | `Clear` | 容器整体重置；同 tick 之前的 op 全部吞掉 |
| 5 | `StructFieldSet` | struct field 模式：单字段更新 |
| 6 | `AddComponent` | 预留（未启用），见 [component_design.md §5](./component_design.md#5-协议预留尚未启用的-op) |
| 7 | `RemoveComponent` | 预留（未启用） |

四 bit 编码上限 16 值，目前 8 个已用 / 占位。

### 4.4 同 tick 合并

`BuildFrame` 里 flush 前做：

| 容器 | 合并规则 |
|---|---|
| list | 相邻 `Set(i)` + `Set(i+1)` → `Splice(i,i+2,[…])`；相邻 `Splice` 合并 |
| dict | 同 key 多次 `Set` 只保留最后一次；`Set(k) → Erase(k)` 整对丢弃 |
| 全量 | 同 tick 出现 `Clear` → 丢弃此前所有 op，合并为 `Clear + Splice(0,0,snapshot)` |
| 字节预算 | 合并后仍超 → 回退 `Clear + Splice(0,0,snapshot)` 整体替换 |

---

## 5. Baseline / Snapshot

容器属性的 baseline / snapshot（`SerializeForOwnerClient` / `SerializeForOtherClients` / `Serialize`）按 **integral** 编码：`[count][element 0][element 1]...`。新观察者进入 AoI / 周期 baseline / DB 持久化都走这条。

op-log 与 integral 是双轨：热增量走 op-log，冷启动 / 重置走 integral。客户端 `ApplyOwnerSnapshot` / `ApplyOtherSnapshot` 反序列化 integral 后直接覆盖 `_items`，不触发 callback（与标量同语义，见 [property_sync_design.md §5](./property_sync_design.md#5-客户端接收侧bigworld-对齐的回调语义)）。

---

## 6. 容错

| 场景 | 行为 |
|---|---|
| 容器越过 `max_size` | 脚本 API 抛 / 返错，丢 op，日志 ERROR |
| 同 tick 合并后仍超字节预算 | 回退 `Clear + Splice(0,0,snapshot)` |
| 嵌套深度 > 8 | DEF010 注册期失败 |
| struct 循环引用 | DEF013 注册期失败 |
| dict key 非标量 | DEF011 |
| 客户端收到非单调 `event_seq` | drop op，gap 计入 `EventSeqGapsTotal`（见 [property_sync_design.md §5](./property_sync_design.md#5-客户端接收侧bigworld-对齐的回调语义)） |

---

## 7. 不做

### 7.1 Merkle diff
AoI 进入 / Cell 迁移 / 冷启动接收方无旧版本，会退化为全量；op-log 已覆盖热路径；发送方维护 leaf/bucket/root hash 状态、热点 dict 写入 O(log n)；客户端调试成本高。

### 7.2 版本号懒 baseline
需要客户端维护 `(entityId, slot) → (version, snapshot)` 缓存子系统（内存上限 / LRU / 重连恢复 / 首次 fallback），命中窗口窄；无线上 profile 数据前属于过早优化。

### 7.3 Component 嵌套 Component
见 [component_design.md §1.2](./component_design.md#12-约束defparser--deflinker-静态校验)。

### 7.4 运行时反射
所有代码走源生成器；不用 `MakeGenericType` / `Emit` / `Expression.Compile`；IL2CPP AOT 安全。

升级触发条件：baseline 字节占比 > 30% 且玩家复联率高 / 存在 5–30 秒短断线不 reap entity 的业务需求 / deflate 后 baseline 仍触顶带宽上限。届时接回不需破坏协议（`event_seq` 已承担定序）。

