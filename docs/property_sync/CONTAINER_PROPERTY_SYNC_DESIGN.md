# 容器属性同步设计

> 初版 2026-04-24 / 最新 2026-04-25（§3.A 登记 kInvalid 哨兵；§6.2 Mutator 改 sealed class + CS1612 背景；§6.3 ItemAt 同步 class）
> 关联: [Entity / Component 设计](./COMPONENT_DESIGN.md) | [属性同步机制设计](./PROPERTY_SYNC_DESIGN.md) | [BigWorld EventHistory 参考](../bigworld_ref/BIGWORLD_EVENT_HISTORY_REFERENCE.md) | [BigWorld RPC 参考](../bigworld_ref/BIGWORLD_RPC_REFERENCE.md)
>
> **快速导航**：想直接看最终决策看 [§3 设计冻结清单](#3-设计冻结清单)；想看 .def 语法看 [§5 .def 文件语法](#5-def-文件语法)；想看协议字节布局看 [§7 线上协议](#7-线上协议)；**Component 体系**（三分类、slot 表、RPC、本地组件、Unity 桥接等）**详见** [Entity / Component 设计](./COMPONENT_DESIGN.md)。

---

## 1. 背景与目标

当前 Atlas 的属性同步（见 [PROPERTY_SYNC_DESIGN.md](./PROPERTY_SYNC_DESIGN.md)）基于 **DirtyBit + 扁平属性**：

- `PropertyDataType` 仅覆盖 15 种基础类型（int/float/string/vector3 等）
- 每个实体用 `_dirtyFlags`（byte/ushort/uint/ulong）标记脏属性，整属性整体序列化
- 所有属性直接挂在 `ServerEntity` 下，无中间层

复杂业务（装备、背包、buff、技能、任务进度等）强依赖**容器类型**（list、dict、struct 及其嵌套）与**模块化组件**。本设计在**不破坏现有扁平属性协议**的前提下，扩展容器类型同步和 Component 组件体系。

### 1.1 目标

1. **类型完备**：支持 `struct` / `list[T]` / `dict[K,V]` 及任意层级嵌套
2. **增量高效**：容器元素级修改以 op-log 形式精确同步，不整体重传
3. **流量极小**：路径与索引按实际容器大小 bit-pack
4. **Unity 兼容**：Mono + IL2CPP AOT 全兼容，零反射，避免泛型膨胀
5. **脚本友好**：`avatar.Bag[3].Count = 5` 这种直观写法直接可用
6. **Component 兼容**：协议字段（`componentIdx` 等）从 P0 起就预留编码位，Component 体系（见 [COMPONENT_DESIGN.md](./COMPONENT_DESIGN.md)）在 P3 启用时无需协议升级
7. **零迁移**：现有 `.def` 文件与 `ServerEntity` 子类不改一行依然工作

### 1.2 非目标

- **Merkle diff**：AoI 进入 / Cell 迁移 / 冷启动场景接收方无旧版本，退化成全量；op-log 已覆盖热路径。拒绝。
- **懒 baseline 缓存**：需要客户端维护 `(entityId, componentIdx) → (version, snapshot)` 缓存子系统，复杂度与收益不匹配。拒绝。见 [§11 明确不做的事](#11-明确不做的事)。
- **运行时反射**：所有代码走源生成器，IL2CPP 友好。

---

## 2. 现状速览

| 层 | 文件 | 关键现状 |
|---|---|---|
| C++ 类型 | `src/lib/entitydef/entity_type_descriptor.h:10-27` | `PropertyDataType` 15 值 + `kCustom=15` 占位 |
| C++ 注册 | `src/lib/entitydef/entity_def_registry.{h,cc}` | 全局单例，C# 侧二进制写入 |
| 序列化 | `src/lib/serialization/binary_stream.{h,cc}` | 小端 + `WritePackedInt` 变长编码 |
| 网络 | `src/lib/network/message_ids.h` | `kReplicatedDeltaFromCell=2015`（不可靠）/`=2017`（可靠）/`=2019`（baseline）|
| 转发 | `src/server/baseapp/baseapp.cc:938-973` | `DeltaForwarder` 最新赢 + 可靠旁路 |
| C# 基类（服务端） | `src/csharp/Atlas.Runtime/Entity/ServerEntity.cs` | `BuildAndConsumeReplicationFrame` 虚方法 |
| C# 基类（客户端） | `src/csharp/Atlas.Client/ClientEntity.cs` | 客户端 Entity 基类，`ApplyReplicatedDelta` 接收端 |
| 源生成器 | `src/csharp/Atlas.Generators.Def/Emitters/DeltaSyncEmitter.cs` | `_dirtyFlags` + `SerializeOwnerDelta/OtherDelta` |

---

## 3. 设计冻结清单

### A. 类型系统

| 条款 | 决定 |
|---|---|
| 扩展 `PropertyDataType` | 新增 `kList=16`、`kDict=17`、`kStruct=18`；保留 `kCustom=15`；`kInvalid=0xFF` 作 default-constructor 哨兵（decoder 硬拒）|
| 递归类型树 | `DataTypeRef { kind; DataTypeRef* elem; DataTypeRef* key; uint16 struct_id }` |
| 结构体描述 | `StructDescriptor { id, name, fields[(name, DataTypeRef)] }` |
| dict key 允许类型 | `string` / `int32` / `uint32` / `int64` / `uint64`（禁 struct/容器作 key） |
| 嵌套深度上限 | 8 层 |
| 容器元素上限 | 属性上 `max_size`，默认 4096 |
| struct 循环引用 | parser 拓扑检测 → 注册期硬失败 |

### B. 运行时同步模型

| 条款 | 决定 |
|---|---|
| Op 种类（必要） | `kSet / kListSplice / kDictSet / kDictErase / kClear / kStructFieldSet` |
| Op 种类（Component 占位） | `kAddComponent / kRemoveComponent`（现在占 enum 位，P3 启用） |
| `opKind` 编码宽度 | 外部线 4 bit（容纳 16 种） |
| struct 同步策略 | 默认 `sync="auto"`，源生成器按 [§9 struct 自动同步判定](#9-struct-自动同步判定) 决定；`sync="whole\|field"` 显式 override |
| 同 tick 合并 | 相邻 `Set`/`Splice` 合并、dict 同 key 覆盖、`Clear` 后 op 全部吸收为 `Clear + Splice(0,0,snapshot)` |
| 快照与 op-log 并存 | baseline（`msg 2019`）承担冷启动，op-log 承担热增量 |
| 定序 | 沿用 `eventSeq`，客户端严格递增 apply，乱序丢弃 |
| 通道 | **必须**走可靠增量 `kReplicatedReliableDeltaFromCell (2017)` → client `0xF003`；**禁**走 `DeltaForwarder` 最新赢通道 |

### C. Component 协议预留

本文档只负责 Component 在**协议层**的占位；完整 Component 设计（三分类、类层级、slot 表、RPC、baseline、本地组件、Unity 桥接等）见 [Entity / Component 设计](./COMPONENT_DESIGN.md)。

| 协议预留点 | 决定 |
|---|---|
| `componentIdx` | 每条 op header 固定编码字段；P0–P2 恒 0，代表 Entity 本体；P3 启用真实 slot |
| `_dirtyComponents` bitmap | `ServerEntity` 字段从 P0 预留；P0–P2 恒 0，不参与 flush |
| `kAddComponent` / `kRemoveComponent` | `OpKind` 枚举位占用（P0 起），P3 启用 |
| Component RPC 路由 | `(entityId, componentIdx, methodIdx)`；P0–P2 `componentIdx=0` 等价现有 Entity RPC |
| Component baseline | `kReplicatedBaselineFromCell` 尾部预留 `[u8 activeComponentCount] + [(slotIdx, snapshot)]*`；P0–P2 `count=0` |

### D. C# 运行时（Unity 兼容）

| 条款 | 决定 |
|---|---|
| struct 值语义保留 | 生成的 struct 仍是 `partial struct`（非 `record struct`，不依赖 C# 10） |
| 字段修改 API | **Mutator Proxy**：`sealed class XxxMutRef` 拦截字段 setter，`implicit operator` 转值读；entity lazy-cache 一个实例 0 per-call 分配（见 §6.2 的 C# 实现约束） |
| 脚本写法 | `avatar.MainWeapon.Count = 5` / `avatar.Bag[3].Count = 5`，**无 `with`**，C# 7.2 可用 |
| 容器具体类型 | 源生成器为每个 `(容器类别, 元素类型)` 生成非泛型具体类（如 `ItemStackObservableList`），避免 IL2CPP 泛型膨胀 |
| list 索引写入 | `list[i]` 索引器返回 `XxxAt`（Mutator），规避 `CS1612` |
| 两级脏位 | `_selfDirtyFlags`（Entity 本体）+ `_dirtyComponents`（Component bitmap），P0–P2 后者恒 0；Component 内部脏位机制详见 [COMPONENT_DESIGN.md §5.2](./COMPONENT_DESIGN.md#52-两级脏状态) |
| GC 目标 | 读写热路径零分配；Mutator 与索引 ref 均为 `readonly struct` |
| 源生成器分发 | `Atlas.Generators.Def` 作为 `RoslynAnalyzer` 打包进 Unity 工程 |

### E. 线上协议（编码头）

外部（客户端方向）bit-pack 布局：

```
[opKind          : 4 bit]
[componentIdx    : bitsRequired(declaredComponentCount + 1) bit]  ← 预留，P0-P2 恒 0
[rootPropIdx     : bitsRequired(propsOfComponent) bit]
[continue-bit = 1 .. path segments .. continue-bit = 0]
[op-specific payload]
```

内部（服务器间）朴素二进制布局：

```
[u8 opKind]
[u8 componentIdx]
[u8 pathLen]
[packed rootPropIdx]
[path segments (packed int per segment)]
[op-specific payload]
```

### F. 不做

- Merkle diff
- 版本号懒 baseline
- 运行时反射
- Component 嵌套 Component

---

## 4. 类型系统扩展

### 4.1 C++ 类型树

`src/lib/entitydef/entity_type_descriptor.h`（示意，非最终代码）：

```cpp
enum class PropertyDataType : uint8_t {
  // 0..14 保留现有基础类型
  kCustom = 15,
  kList   = 16,
  kDict   = 17,
  kStruct = 18,
  // Sentinel — default-constructed DataTypeRef.kind. Decoders reject it
  // so the wire can't smuggle a "未初始化" blob through ValidatePropertyInvariant.
  kInvalid = 0xFF,
};

struct DataTypeRef {
  PropertyDataType kind;
  uint16_t struct_id{0};                   // kStruct: 指向 StructDescriptor
  std::unique_ptr<DataTypeRef> elem;       // kList: 元素类型；kDict: value 类型
  std::unique_ptr<DataTypeRef> key;        // kDict: key 类型
};

struct StructDescriptor {
  uint16_t id;
  std::string name;
  std::vector<std::pair<std::string, DataTypeRef>> fields;  // 顺序 = 字段索引
};

struct PropertyDescriptor {
  // 现有字段 (name/scope/persistent/reliable/...) 保留
  DataTypeRef type;                         // 取代裸 PropertyDataType
  uint32_t max_size{4096};                  // 容器属性专用，标量属性忽略
};
```

`EntityDefRegistry` 新增 `RegisterStruct()` / `ResolveStruct(id)`。（`RegisterComponent()` 见 [COMPONENT_DESIGN.md §10](./COMPONENT_DESIGN.md#10-涉及的模块与文件)）

### 4.2 C# 类型映射

源生成器扩展 `DefTypeHelper`：

| .def 类型 | C# 类型（字段） | C# 类型（属性返回） |
|---|---|---|
| `int32` | `int` | `int` |
| `ItemStack`（struct） | `ItemStack`（struct） | `ItemStackMutRef`（readonly struct） |
| `list[ItemStack]` | `ItemStackObservableList`（class） | `ItemStackObservableList`（引用） |
| `dict[string,ItemStack]` | `StringItemStackObservableDict`（class） | 同左 |
| `list[list[int32]]` | `Int32ListObservableList`（具体类） | 同左 |

---

## 5. .def 文件语法

### 5.1 类型字符串递归语法

```
<type-expr> ::= <base-type>                             // int32, string, vector3, ...
              | <struct-name>                           // 已声明的 struct / alias
              | "list[" <type-expr> "]"
              | "dict[" <key-type> "," <type-expr> "]"
<key-type>  ::= string | int32 | uint32 | int64 | uint64
```

空格宽松：`list[int32]` ≡ `list[ int32 ]`。嵌套不限（上限 8 层）。方括号选用是因为 .def 是 XML，避免 `&lt;` / `&gt;` 转义。

### 5.2 `<types>` 段：struct 与 alias

```xml
<entity name="Avatar">
  <types>
    <struct name="ItemStack">
      <field name="id"    type="int32"  />
      <field name="count" type="uint16" />
      <field name="bound" type="bool"   default="false" />
    </struct>

    <struct name="SkillEntry">
      <field name="id"    type="uint16" />
      <field name="level" type="uint8"  />
      <field name="cd"    type="float"  />
    </struct>

    <alias name="Inventory" type="list[ItemStack]" />
  </types>

  <properties>
    <property name="hp"   type="int32"     scope="all_clients" reliable="true" />
    <property name="bag"  type="Inventory" scope="own_client"  persistent="true"
                                           max_size="128" />
    <property name="equipped"
              type="dict[string,ItemStack]"
              scope="all_clients" persistent="true"
              max_size="64" />
  </properties>
</entity>
```

容器属性可选 `max_size` 属性（默认 4096），Parser 在写入时校验元素数；越界走 [§10 容错](#10-约束容错与边界) 的回退路径。

### 5.3 struct 字段约束

- 字段不允许 `scope` / `persistent` / `reliable`：继承所在属性
- 允许 `default="..."`：用于向后兼容新增字段
- 可选顶层 `sync="auto|whole|field"`（默认 `auto`，详见 [§9](#9-struct-自动同步判定)）

### 5.4 跨实体共享：`entity_defs/types.xml`

```xml
<types>
  <struct name="Vec2">
    <field name="x" type="float" />
    <field name="y" type="float" />
  </struct>
  <alias name="ItemList" type="list[ItemStack]" />
</types>
```

`DefParser` 启动时先加载此文件；各 `entity.def` 的 `<types>` 段覆盖/补充全局表。

### 5.5 Component 声明（跳转）

Entity 内 `<components>` 段、独立 `component.def` 文件、`local="server\|client"` 属性、parser 校验规则等，详见 [Entity / Component 设计 §3 Component 声明语法](./COMPONENT_DESIGN.md#3-component-声明语法)。

本文档仅关心一件事：Component 内的属性可用本节前述的 `list[T]` / `dict[K,V]` / struct 类型语法，与 Entity 属性完全一致。

---

## 6. 运行时模型

### 6.1 所有权链

```
ServerEntity  (TopLevelPropertyOwner)
     │
     ├── (扁平属性) — 通过 _selfDirtyFlags
     │
     └── [P3+] Component[i]  (IPropertyOwner，详见 COMPONENT_DESIGN.md)
             │
             └── ObservableList / ObservableDict  (IPropertyOwner)
                     │
                     └── 元素 (struct Mutator 或 scalar)
```

叶子节点的 mutation 沿链向上冒泡：每级 `IPropertyOwner.AddPath()` 把自己在父中的索引追加到 `OpRecord.path`，最终由 `ServerEntity` 写入本 tick 的 op 缓冲。Component 作为链中的一层，其职责与接入方式详见 [COMPONENT_DESIGN.md §4 类层级与生命周期](./COMPONENT_DESIGN.md#4-类层级与生命周期)。

### 6.2 Mutator Proxy（struct 字段修改）

见 [§3 D 条](#d-c-运行时unity-兼容)。核心生成代码：

```csharp
// 生成的 struct 保持值语义
public partial struct ItemStack
{
    public int    Id;
    public ushort Count;
    public bool   Bound;
}

// 生成到 Avatar 上的属性（MutRef 嵌套在 Avatar 内以访问 private 字段）
public partial class Avatar
{
    private ItemStack _mainWeapon;
    // Lazy-initialised cache — per-call access is 0-alloc; only the first
    // access for a given (entity, struct-prop) pair allocates the MutRef.
    private MainWeaponMutRef? __mainWeaponMutRef;
    public MainWeaponMutRef MainWeapon => __mainWeaponMutRef ??= new(this);
    public ItemStack MainWeaponValue => _mainWeapon;   // 显式取快照

    public sealed class MainWeaponMutRef
    {
        private readonly Avatar _owner;
        internal MainWeaponMutRef(Avatar o) { _owner = o; }

        public ushort Count
        {
            get => _owner._mainWeapon.Count;
            set {
                if (_owner._mainWeapon.Count == value) return;
                _owner._mainWeapon.Count = value;
                _owner._dirtyFlags |= ReplicatedDirtyFlags.MainWeapon;
            }
        }
        // ...

        public static implicit operator ItemStack(MainWeaponMutRef r)
            => r._owner._mainWeapon;
    }
}
```

> **为什么是 class 而不是 readonly struct**
> 最初的设想是 `readonly struct XxxMutRef`（无堆分配）。实装时发现 C# CS1612
> 对任何**按值返回**的 struct 字段赋值均拒绝，不区分 `readonly set` 是否触碰
> `this` —— 编译器没法验证 setter 的副作用范围，保守报错。改为 `sealed class`
> 后由 entity 本身 lazy-cache 一个实例：首次访问分配 24 B，后续完全 0 allocation
> （见 `StructSyncTests.MutRef_*` 三条守卫测试）。5000 entity × ~3 个 struct 属性
> ≈ 360 KB 长期常驻堆，对 Unity Mono / IL2CPP 可忽略。P1 的 `list[struct]` 元素
> mutator 同理（class pool），复用这一模式。

脚本端：

```csharp
avatar.MainWeapon.Count = 5;            // setter 拦截 + 标脏
ItemStack snap = avatar.MainWeapon;     // 隐式转值
```

### 6.3 ObservableContainer（容器修改）

对每个 `(容器类别, 元素类型)` 生成非泛型具体类：

```csharp
public sealed class ItemStackObservableList : IPropertyOwner
{
    private readonly List<ItemStack> _items = new();
    private IPropertyOwner _parent;
    private int _ownerRef;

    public int Count => _items.Count;
    public ItemStack Get(int i) => _items[i];                   // 只读取
    public ItemAt this[int i] => new(this, i);                  // 写：索引返回 Mutator
    public ReadOnlySpan<ItemStack> AsReadOnlySpan() => CollectionsMarshal.AsSpan(_items);

    public void Add(ItemStack v) {
        _items.Add(v);
        _parent.OnChildMutated(OpRecord.Splice(_ownerRef, _items.Count - 1, _items.Count - 1, v));
    }
    public void RemoveAt(int i) { /* Splice(i, i+1, []) */ }
    public void Clear()         { /* kClear */ }

    internal void __WriteBack(int i, ItemStack v) {
        _items[i] = v;
        _parent.OnChildMutated(OpRecord.Set(_ownerRef, i, /*serialize v*/));
    }
}

// Same CS1612 reasoning as §6.2 — `ItemAt` lands as a sealed class so
// `list[i].Count = 5` compiles. Pool the instances per-list to keep
// per-access allocation at zero (see P1 implementation for details).
public sealed class ItemAt
{
    private readonly ItemStackObservableList _list;
    private int _idx;  // list can rebind the accessor to a different slot

    public ushort Count {
        get { var s = _list.Get(_idx); return s.Count; }
        set { var s = _list.Get(_idx); s.Count = value; _list.__WriteBack(_idx, s); }
    }
    // ...
    public static implicit operator ItemStack(ItemAt r) => r._list.Get(r._idx);
}
```

### 6.4 两级脏状态（预留）

`ServerEntity` 从 P0 起就分别持有：

- `_selfDirtyFlags`（Entity 本体扁平属性的脏位，沿用现状）
- `_dirtyComponents`（Component 脏位 bitmap，P0–P2 恒 0）

P0–P2 期间 `_dirtyComponents == 0` 恒成立，fast path 零开销。Component 聚合逻辑详见 [COMPONENT_DESIGN.md §5.2 两级脏状态](./COMPONENT_DESIGN.md#52-两级脏状态)。

### 6.5 同 tick op 合并

在 `BuildFrame` 里 flush 前做：

| 容器 | 合并规则 |
|---|---|
| list | 相邻 `Set(i)` + `Set(i+1)` → `Splice(i,i+2,[…])`；`Splice(a,b,…) + Splice(b,c,…)` → 合并为单 `Splice(a,c,…)` |
| dict | 同 key 多次 `Set` 只保留最后一次；`Set(k) → Erase(k)` 整体丢弃 |
| 全量 | 同 tick 出现 `Clear` → 丢弃此前所有 op，合并为 `Clear + Splice(0,0,snapshot)` |
| 字节预算 | 合并后仍 > 预算 → 回退 `Clear + Splice(0,0,snapshot)` 整体替换 |

---

## 7. 线上协议

### 7.1 消息通道

| 场景 | 通道 | 消息 ID | 备注 |
|---|---|---|---|
| 容器 op-log | 可靠 | `kReplicatedReliableDeltaFromCell = 2017` → client `0xF003` | 丢一帧即状态发散，**禁走** `DeltaForwarder` |
| 可靠扁平属性增量（`reliable="true"`） | 可靠 | 同上 `2017` → `0xF003` | Atlas 现状：已走此通道，本设计搭便车同帧 |
| 不可靠扁平属性增量（默认） | 不可靠（经 `DeltaForwarder` 最新赢） | `kReplicatedDeltaFromCell = 2015` → `0xF001` | 现状不变，与容器无交集 |
| AoI 进入 / 冷启动 baseline | 可靠 | `kReplicatedBaselineFromCell = 2019` | 首次装配整实体 |
| volatile 位置/方向 | 不可靠 | `2015` → `0xF001` | 独立通道，与属性无关 |

### 7.2 帧内分段（仅 `msg 2017` 可靠通道）

可靠通道 `kReplicatedReliableDeltaFromCell` 的一帧可同时承载**两类**数据：reliable 扁平属性与容器 op-log。不可靠通道 `2015` 布局不变，与本节无关。

```
[u64 eventSeq]
[u8  sectionMask]                  ← bit0=Scalar, bit1=ContainerOp
[Scalar  section if bit0]          = reliable 扁平属性 delta
                                     (仅 `reliable="true"` 的标量属性；
                                      unreliable 默认走 msg 2015)
[Container section if bit1]        = 容器 op-log (按 §7.3 编码)
```

两段均可为空（对应 sectionMask 位清零），不强制共存。

### 7.3 Op 外部线布局（bit-pack 到客户端）

```
[opKind          : 4 bit]
[componentIdx    : bitsRequired(declaredComponentCount + 1) bit]
[rootPropIdx     : bitsRequired(propsOfComp) bit]

loop:
  [continue = 1 : 1 bit]
  [childIdx  : bitsRequired(containerSize) bit]
结束:
  [continue = 0 : 1 bit]

[op-specific payload]
```

### 7.4 各 op 的 payload

| opKind | Payload（外部线） |
|---|---|
| `kSet` | `[leafIdx : bitsRequired(parentSize)]` + `[value bytes]` |
| `kListSplice` | `[start : bitsRequired(size+1)]` `[end : bitsRequired(size+1)]` `[count : packed_int]` `[newValues...]` |
| `kDictSet` | `[key bytes]` + `[value bytes]` |
| `kDictErase` | `[key bytes]` |
| `kClear` | （空） |
| `kStructFieldSet` | `[fieldIdx : bitsRequired(fieldCount)]` + `[value bytes]` |
| `kAddComponent`（P3） | `[componentTypeId : 16 bit]` + `[initial snapshot bytes]` |
| `kRemoveComponent`（P3） | （空，componentIdx 在头里） |

### 7.5 典型包体尺寸估算

| 改动 | 外部字节 |
|---|---|
| `avatar.Hp = 100`（扁平 int32） | 1B flags + 4B = 5B |
| `avatar.MainWeapon.Count = 5`（struct whole，通过 Mutator） | 1B flags + 7B = 8B |
| `avatar.Bag[3].Count = 5`（list 中元素字段） | ~10B |
| `avatar.Combat.Skills[2].Id = 1001`（Component→list→struct 字段） | ~8-10B header + 4B value = 12-14B |
| `avatar.Combat.Skills.Add(entry)` | ~3-4B header + 7B struct = 10-11B |

对照整体重传（如 list 有 50 个元素、每个 7B = 350B），节省 > 95%。

---


## 8. Component 扩展（跳转）

Component 体系（三分类、类层级、静态 slot 表、动态 Add/Remove、两级 scope 过滤、Component RPC、Component baseline、本地组件、Unity 桥接）详见：

**[Entity / Component 设计](./COMPONENT_DESIGN.md)**

本文档对 Component 的承接点集中在 [§3.C Component 协议预留](#c-component-协议预留) 与 [§7 线上协议](#7-线上协议) 的 `componentIdx` 字段，P0–P2 期间这些字段恒 0，P3 启用时**无需协议升级**。

---

## 9. struct 自动同步判定

源生成器按**从上到下短路**规则：

| # | 条件 | 判定 |
|---|---|---|
| 1 | 字段内含变长类型（`string` / `bytes` / `list` / `dict` / 嵌套 struct） | **whole** |
| 2 | 定长总字节 `S ≤ 8B` | **whole** |
| 3 | 字段数 `N ≤ 4` | **whole** |
| 4 | `S ≥ 32B` **且** `N ≥ 8` | **field** |
| 5 | 中间地带 | **whole**（保守默认） |

### 9.1 为什么保守偏 whole

Unity Mono/IL2CPP 下：

- **field 同步 = 生成 class 而非 struct**，嵌套在 `list[T]` 里每次 `Add` 触发 GC，Mono 下代价高于多发几十字节
- **field 同步 = 每次 setter 走一次 `_parent.OnChildMutated`**，op 合并负担更重

只在 `S ≥ 32B & N ≥ 8` 翻转为 field。

### 9.2 Build-time 诊断

生成器用 Roslyn `DiagnosticDescriptor`（`Info` 级别）输出每个 struct 的判定：

```
[Atlas.Generators.Def] INFO: struct ItemStack       → whole (S=7B, N=3, 定长)
[Atlas.Generators.Def] INFO: struct CharacterStats  → field (S=80B, N=20, 定长)
[Atlas.Generators.Def] INFO: struct QuestProgress   → whole (含 list[int32], N=4)
```

### 9.3 显式 override

```xml
<struct name="BuffInst" sync="field">
  <field name="id"    type="uint32" />
  <field name="stack" type="uint8" />
  <field name="end"   type="float" />
</struct>
```

`sync="auto|whole|field"`，默认 `auto`。

---

## 10. 约束、容错与边界

| 场景 | 行为 |
|---|---|
| 容器越过 `max_size` | 脚本 API 返回 `Result<Err>`，日志 ERROR，丢弃 op |
| 合并后仍超字节预算 | 回退 `Clear + Splice(0,0,snapshot)` |
| 嵌套深度 > 8 | 类型注册期失败 |
| struct 循环引用 | 注册期失败 |
| 客户端收到非单调 `eventSeq` | 丢弃 op，日志 WARN |
| 持久化（DB） | 整体快照 `Serialize()`，与增量解耦，现有 DBApp 路径不变 |

> Component 相关约束（`P.scope ⊆ C.scope`、`AddComponent<T>` 未声明、`componentIdx` 超出已知 slot 等）见 [COMPONENT_DESIGN.md §8](./COMPONENT_DESIGN.md#8-约束与容错)。

---

## 11. 明确不做的事

### 11.1 Merkle diff

**拒绝**。理由：

- AoI 首次进入 / Cell 迁移 / 冷启动 → 接收方无旧版本，Merkle 退化为全量
- 短断线重连场景可用 deflate 全量替代
- 发送方需维护 leaf/bucket/root hash 状态，热点 dict 每次写入 O(log n)
- 客户端复杂度、内存、调试成本高
- op-log 已覆盖热路径

### 11.2 版本号懒 baseline

**拒绝**。理由：

- 看似加一个 `u32 version`，实际需要客户端维护 `(entityId, componentIdx) → (version, snapshot)` 缓存子系统（内存上限 / LRU / 重连恢复 / 首次 fallback）
- 短断线场景在 Atlas 常伴随 AoI 重建与 entity reap，命中窗口窄
- 无线上 profile 数据前属于过早优化

### 11.3 Component 嵌套 Component

**拒绝**。详见 [COMPONENT_DESIGN.md §2 冻结清单](./COMPONENT_DESIGN.md#2-设计冻结清单) 的 "Component 嵌套" 条款。

### 11.4 运行时反射

**拒绝**。所有代码走源生成器。`MakeGenericType` / `Emit` / `Expression.Compile` 均不用，IL2CPP AOT 安全。

### 11.5 升级触发条件

以下 metrics 出现时，单独评审是否接回 §11.1 / §11.2：

- baseline 字节占比 > 30% 且玩家复联率高
- 存在 "5–30 秒短断线不 reap entity" 业务需求
- deflate 后 baseline 仍触顶带宽上限

届时接回不需要破坏协议：`eventSeq` 已承担定序，懒 baseline 只是多一个"要不要发"的判断。

---

## 12. 分阶段实施

| 阶段 | 范围 | 关键交付 |
|---|---|---|
| **P0** | 标量 struct（无容器，`sync=whole` 判定路径） | `DataTypeRef`/`StructDescriptor`/`StructEmitter`/`MutRef`；`componentIdx` 协议字段预留（恒 0）；`_dirtyComponents` 字段预留（恒 0） |
| **P1** | `list[T]` / `dict[K,V]` 整体同步 + op-log + 同 tick 合并 | `ObservableList`/`ObservableDict` 具体类生成；op 编解码；可靠通道集成；容器 baseline |
| **P2** | 嵌套容器；`kStructFieldSet`（`sync=field`） | 递归 `DataTypeRef`；嵌套 op 路径；字段级 Observable class；自动判定规则上线 |
| **P3** | Component 体系（详见 [COMPONENT_DESIGN.md §9](./COMPONENT_DESIGN.md#9-分阶段实施)） | slot 表、动态 Add/Remove、Witness 两级过滤、Component RPC、本地 Component 两种激活方式 |
| **P4** | 外部线 bit-pack 压缩 | `BitWriter`/`BitReader` 复用；ops 改压缩路径；协议版本号（兼容层） |
| **P5+** | （条件触发）懒 baseline / Merkle | 依 [§11.5](#115-升级触发条件) metrics 决策 |

---

## 13. 涉及的模块与文件

### C++

| 文件 | 变化 |
|---|---|
| `src/lib/entitydef/entity_type_descriptor.h` | `PropertyDataType` 扩容；引入 `DataTypeRef`；`PropertyDescriptor::type` 升级 |
| `src/lib/entitydef/struct_descriptor.{h,cc}` | 新增 |
| `src/lib/entitydef/entity_def_registry.{h,cc}` | `RegisterStruct`；二进制描述符版本升级 |
| `src/lib/serialization/bit_stream.{h,cc}` | 新增（P4） |
| `src/lib/network/message_ids.h` | 现有 2017/2019 不变 |
| `src/server/baseapp/baseapp.cc` | `OnReplicatedReliableDeltaFromCell` 透传 Container 段 |
| `src/server/cellapp/cell_entity.{h,cc}` | ReplicationState 承载 op buffer |

> Component 相关的 C++ 改动（`component_descriptor.{h,cc}`、`RegisterComponent`、Component RPC dispatch 等）见 [COMPONENT_DESIGN.md §10](./COMPONENT_DESIGN.md#10-涉及的模块与文件)。

### C#

| 文件 | 变化 |
|---|---|
| `src/csharp/Atlas.Runtime/Observable/ObservableList_*.cs` | 源生成器产出（具体类型，非泛型） |
| `src/csharp/Atlas.Runtime/Observable/ObservableDict_*.cs` | 同上 |
| `src/csharp/Atlas.Runtime/Observable/IPropertyOwner.cs` | 新增接口 |
| `src/csharp/Atlas.Runtime/Observable/OpRecord.cs` | 新增 |
| `src/csharp/Atlas.Runtime/Observable/OpCodec.cs` | 编解码 |
| `src/csharp/Atlas.Runtime/Entity/ServerEntity.cs` | `_selfDirtyFlags` / `_dirtyComponents` 字段预留 |
| `src/csharp/Atlas.Generators.Def/DefParser.cs` | 类型字符串递归解析；`<types>` 段 |
| `src/csharp/Atlas.Generators.Def/DefTypeHelper.cs` | 扩展类型映射 |
| `src/csharp/Atlas.Generators.Def/Emitters/StructEmitter.cs` | 新增（生成 struct + MutRef + auto sync 判定） |
| `src/csharp/Atlas.Generators.Def/Emitters/ContainerEmitter.cs` | 新增（生成具体 Observable 类） |
| `src/csharp/Atlas.Generators.Def/Emitters/DeltaSyncEmitter.cs` | 扩展：容器字段不进 `_selfDirtyFlags` |

> Component 相关的 C# 改动（四基类、`<components>` 段 parser、`ComponentEmitter`、Entity 访问器、`DeltaSyncEmitter` 的 Component 汇聚）见 [COMPONENT_DESIGN.md §10](./COMPONENT_DESIGN.md#10-涉及的模块与文件)。

---

## 14. 未决项（不阻塞 P0/P1）

- 大 dict 的 chunked baseline（dict 单属性 baseline 字节超过一个消息上限时是否切分 —— 预计在 P1 压测后评估）

> Component 相关未决项（跨 slot flush 顺序、ServerLocal 跨 CellApp 迁移、AddLocalComponent 构造注入）见 [COMPONENT_DESIGN.md §11](./COMPONENT_DESIGN.md#11-未决项不阻塞-p0p1)。

### 已解决（按拍板时间倒序）

- **2026-04-24**：Component 相关设计（三分类、slot 表、RPC、本地组件、Unity 桥接等）整体拆出至独立文档 [COMPONENT_DESIGN.md](./COMPONENT_DESIGN.md)。
