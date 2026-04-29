# Entity / Component 设计

> 初版 2026-04-24 / 最新 2026-04-24（§4.2 OnTick 调用方；§5.6 RPC 线上布局；§6 anchor 修复；emoji 清理）
> 关联: [容器属性同步与 Component 扩展设计](./CONTAINER_PROPERTY_SYNC_DESIGN.md) | [属性同步机制设计](./PROPERTY_SYNC_DESIGN.md) | [BigWorld RPC 参考](../bigworld_ref/BIGWORLD_RPC_REFERENCE.md)
>
> **快速导航**：想直接看最终决策看 [§2 冻结清单](#2-设计冻结清单)；想看 `.def` 语法看 [§3 Component 声明语法](#3-component-声明语法)；想看 Synced Component 的协议与 RPC 看 [§5 Synced Component](#5-synced-component)；想看 Unity 侧本地组件看 [§6 本地 Component](#6-本地-component-serverlocal--clientlocal)。

---

## 1. 背景与目标

Atlas 的 Entity 当前只有扁平属性。随着业务规模上升（战斗、背包、任务、动画、特效、AI、掉落、反作弊、UI 绑定……）需要**模块化组件**来组织同端与跨端逻辑。设计上要同时满足：

1. **同步组件**由 `.def` 声明，双端代码结构对称，属性/RPC 通过协议同步
2. **服务端本地组件**（AI、LootRoller、AntiCheat 等）只在服务端存在，不占协议带宽
3. **客户端本地组件**（AnimControl、VFX、UIBinder、InputPredictor 等）只在 Unity 侧存在
4. **Unity 兼容**：Mono + IL2CPP AOT、零反射、无泛型膨胀
5. **协议预留**：同步层把 `componentIdx` 作为协议字段从 P0 起就编码，P3 启用 Component 不需要破坏协议

### 1.1 三分类（核心区分）

| 维度 | Synced | ServerLocal | ClientLocal |
|---|---|---|---|
| 来源 | `.def` 声明 | `.def` `local="server"` / C# API 动态挂 | `.def` `local="client"` / C# API 动态挂 |
| 参与协议 | 是（占 `componentIdx`） | 否 | 否 |
| 写入 op-log | 是 | 否 | 否 |
| RPC 路由 | `(entityId, componentIdx, methodIdx)` | 纯本地 C# 调用 | 纯本地 C# 调用 |
| 生命周期 | 双端对称 | 仅服务端 | 仅客户端 |
| 典型例子 | Combat, Inventory, Quest | ServerAI, LootRoller, AntiCheat, ChronicleRecorder | AnimControl, VFX, AudioSource, UIBinder, InputPredictor |

**核心洞察**：`componentIdx` 只对 Synced 有意义；本地 component **不能污染** slot 表，否则线上编码宽度飘移、双端不一致。

---

## 2. 设计冻结清单

| 条款 | 决定 |
|---|---|
| 三分类 | **Synced** / **ServerLocal** / **ClientLocal**。仅 Synced 参与协议 |
| 类层级 | `ComponentBase`（生命周期） → `ReplicatedComponent`（Synced，`IPropertyOwner`） / `ServerLocalComponent` / `ClientLocalComponent`，双端对称 |
| 静态 slot | **仅 Synced** 必须在 `.def` 声明；`AddComponent<T>()` 只激活已声明 Synced slot；类型注册期固化 slot 编号与编码宽度 |
| `componentIdx=0` | 永久保留给 Entity 本体；用户 Component 索引从 1 起 |
| Slot 生命周期 | `Declared → Active → Declared`，**不回收**，slot 编号永远稳定 |
| Lazy | `.def` `lazy="true"` → 默认 `Declared`（等 `AddComponent`）；否则 `Active`（Entity 构造时自动实例化） |
| 两级 scope | `observer 可见 ⟺ observer ∈ C.scope ∧ observer ∈ P.scope`；parser 硬校验 `P.scope ⊆ C.scope` |
| Component RPC | **允许**。路由 `(entityId, componentIdx, methodIdx)`；`componentIdx=0` 走 Entity 本体（向后兼容） |
| RPC scope 约束 | `method.exposed ⊆ component.scope`；parser 注册期硬校验，违反失败 |
| Component 嵌套 | **禁止**。Slot 表扁平，组合业务用 `list[struct]` |
| 位置一致性 | 同一 Component 的属性必须全在 Cell 或全在 Base，不允许跨进程 |
| 同 tick `AddComponent + 改属性` 顺序 | 先 `kAddComponent`（含 Component 初始快照），再该 slot 的属性 op |
| 新观察者进 AoI | 按 slot 顺序遍历已激活 Component 的快照塞进 baseline（`msg 2019`），**不发** `kAddComponent` |
| 本地 component 占 slot 表 | **不占**。只 Synced 计数 |
| 本地 component 属性 / RPC | **禁止**在 `.def` 声明 `<properties>` / `<methods>`；本地状态与方法直接写在 C# 类 |
| 源生成器按目标分流 | MSBuild 属性 `AtlasComponentTarget=Server\|Client` 控制产出 |
| 本地 component 动态挂载 | C# API `AddLocalComponent<T>()` 允许不在 `.def` 声明的类型 |
| Unity 集成 | `ClientLocalComponent` 纯 POCO，不继承 `MonoBehaviour`；需要 GameObject 通过 `IGameObjectBinding` 接口 |

---

## 3. Component 声明语法

### 3.1 Entity 内 `<components>` 段

三类 Component 统一在 `<components>` 段声明，`local` 属性区分：

```xml
<entity name="Avatar">
  <properties>
    <property name="hp" type="int32" scope="all_clients" reliable="true" />
  </properties>

  <components>
    <!-- Synced: 不写 local 属性，默认参与同步 slot 表 -->
    <component name="combat"    type="CombatComponent"    />
    <component name="inventory" type="InventoryComponent" scope="own_client" />
    <component name="quest"     type="QuestComponent"     lazy="true" />

    <!-- ServerLocal: 仅服务端实例化，不占 componentIdx -->
    <component name="ai"         type="ServerAIComponent"   local="server" />
    <component name="lootRoller" type="LootRollerComponent" local="server" />

    <!-- ClientLocal: 仅客户端实例化，不占 componentIdx -->
    <component name="anim"     type="AnimControlComponent" local="client" />
    <component name="vfx"      type="VFXComponent"         local="client" />
    <component name="uiBinder" type="UIBinderComponent"    local="client" />
  </components>
</entity>
```

**`local` 属性语义**：

| 值 | 语义 |
|---|---|
| 缺省 | **Synced**，占 `componentIdx`，参与协议（默认） |
| `local="server"` | **ServerLocal**，仅服务端工程生成；客户端源生成器跳过 |
| `local="client"` | **ClientLocal**，仅客户端工程生成；服务端源生成器跳过 |

### 3.2 独立 `component.def` 文件（仅 Synced）

```xml
<component name="CombatComponent" location="cell">
  <properties>
    <property name="atk"    type="int32" scope="all_clients" />
    <property name="skills" type="list[SkillEntry]" scope="own_client" />
    <property name="buffs"  type="dict[uint32,BuffInst]" scope="all_clients" />
  </properties>

  <client_methods>
    <method name="OnSkillEffect">
      <arg name="skillId"  type="int32" />
      <arg name="casterId" type="uint32" />
    </method>
  </client_methods>

  <cell_methods>
    <method name="CastSkill" exposed="own_client">
      <arg name="skillId" type="int32" />
    </method>
  </cell_methods>
</component>
```

### 3.3 约束（parser 静态校验）

- `location="cell"` / `location="base"`（仅 Synced）：Component 的属性位置侧必须一致
- 属性 `P.scope ⊆ C.scope`（仅 Synced）：不满足则注册期失败
- RPC `method.exposed ⊆ component.scope`（仅 Synced）：不满足则注册期失败（与属性规则同构）
- Component 不能嵌套 Component
- `<component name="combat">` 的 `name` 是 slot 名（C# 属性名），`type` 是 Component 类型名
- **本地 component 禁止** `<properties>` / `<methods>`：本地状态与方法直接写在 C# 类；违反则 parser 硬失败
- **本地 component 的 `scope` 属性无意义**：parser 输出 Info 诊断并忽略
- 本地 component 的 `name`（slot 名）不得与 Synced component 冲突

---

## 4. 类层级与生命周期

### 4.1 基类

```csharp
// Atlas.Runtime/Entity/Components/ComponentBase.cs
public abstract class ComponentBase
{
    internal ServerEntity _entity = null!;
    public ServerEntity Entity => _entity;

    public virtual void OnAttached() { }
    public virtual void OnDetached() { }
    public virtual void OnTick(float dt) { }
}

// 同步组件（.def 所有 Synced component 继承它）
public abstract class ReplicatedComponent : ComponentBase, IPropertyOwner
{
    internal int _slotIdx;                  // componentIdx，注册期固化
    private protected ulong _dirtyFlags;

    protected void MarkDirty(int propIdx)
    {
        _dirtyFlags |= 1UL << propIdx;
        _entity.__MarkComponentDirty(_slotIdx);
    }

    internal abstract bool BuildFrame(
        ref SpanWriter ownerSnap, ref SpanWriter otherSnap,
        ref SpanWriter ownerDelta, ref SpanWriter otherDelta);
}

// 服务端本地
public abstract class ServerLocalComponent : ComponentBase { }

// 客户端本地
public abstract class ClientLocalComponent : ComponentBase { }
```

### 4.2 生命周期钩子共享

所有 Component 共享 `OnAttached` / `OnDetached` / `OnTick`。仅 `ReplicatedComponent` 有 `BuildFrame`（服务端）/ `ApplyDelta`（客户端生成器产出）等同步钩子。

**调用方**：

- `OnAttached` — Entity 创建 / `AddComponent<T>()` / `AddLocalComponent<T>()` 时由 Entity 同步调
- `OnDetached` — Entity 析构（按 slot 倒序，先 Synced 后 Local）/ `RemoveComponent<T>()` 时由 Entity 同步调
- `OnTick(dt)` — 服务端由 CellApp tick loop 在 Entity tick 阶段调 `entity.TickAllComponents(dt)`：先按 slot 升序遍历 Active Synced，再按挂载顺序遍历 `_serverLocal`；客户端由 `ClientEntityManager` tick 驱动，策略相同（Active Synced → `_clientLocal`）

### 4.3 Entity 持有三套存储

```csharp
public abstract class ServerEntity
{
    // Synced — 静态 slot 表，索引就是 componentIdx
    private protected ReplicatedComponent?[] _replicated = null!;
    private protected ulong _dirtyComponents;

    // ServerLocal — 动态，按 Type dispatch，不参与协议
    private readonly Dictionary<Type, ServerLocalComponent> _serverLocal = new();

    public T? GetComponent<T>() where T : ComponentBase
    {
        // 优先查 Synced 的强类型访问器（O(1)），再查 _serverLocal
    }

    public T AddLocalComponent<T>() where T : ServerLocalComponent, new()
    { /* 无网络副作用 */ }

    public void RemoveLocalComponent<T>() where T : ServerLocalComponent
    { /* 无网络副作用 */ }
}
```

客户端 `ClientEntity`（`src/csharp/Atlas.Client/ClientEntity.cs`）对称持有 `_replicated + _clientLocal`。**双端 `_replicated` slot 表完全一致**（都来自 .def），local 各自独立。

### 4.4 强类型访问器（零反射）

源生成器为每个 Entity 类按其 `<components>` 段产出具名属性：

```csharp
public partial class Avatar : ServerEntity
{
    // Synced: O(1) slot 索引，返回具体类型
    public CombatComponent    Combat    => (CombatComponent)_replicated[1]!;
    public InventoryComponent Inventory => (InventoryComponent)_replicated[2]!;
    public QuestComponent?    Quest     => _replicated[3] as QuestComponent; // lazy

    // ServerLocal: 按 Type 查
    public ServerAIComponent? Ai => GetComponent<ServerAIComponent>();
}
```

脚本写 `avatar.Combat.Atk = 50` 或 `avatar.Anim.Play("idle")` 统一语法，运行时零反射、零装箱。

---

## 5. Synced Component

### 5.1 静态 slot 表

`EntityTypeDescriptor.components[]` 在类型注册时固化。运行时：

```
Slot 0                   → Entity 本体（保留）
Slot 1..N (Active)       → 已挂载的 Synced Component 实例
Slot 1..N (Declared)     → 已声明但未激活（lazy 或 Remove 后）
```

slot 数组大小 = `declared Synced count + 1`，`componentIdx` 编码宽度 = `bitsRequired(declaredCount + 1)`，注册期固定。

### 5.2 两级脏状态

```csharp
public abstract class ServerEntity
{
    private protected ulong _selfDirtyFlags;         // Entity 本体属性
    private protected ulong _dirtyComponents;        // 哪些 Component 脏了 (bitmap)
    private protected ReplicatedComponent?[] _replicated;

    public override bool BuildAndConsumeReplicationFrame(
        ref SpanWriter ownerSnap, ref SpanWriter otherSnap,
        ref SpanWriter ownerDelta, ref SpanWriter otherDelta,
        out ulong eventSeq, out ulong volatileSeq)
    {
        bool any = BuildSelfFrame(ref ownerSnap, ref otherSnap, ref ownerDelta, ref otherDelta);
        if (_dirtyComponents != 0) {
            for (int i = 0; i < _replicated.Length; ++i) {
                if ((_dirtyComponents & (1UL << i)) == 0) continue;
                any |= _replicated[i]!.BuildFrame(ref ownerSnap, ref otherSnap,
                                                  ref ownerDelta, ref otherDelta);
            }
            _dirtyComponents = 0;
        }
        if (any) _eventSeq++;
        eventSeq = any ? _eventSeq : 0;
        volatileSeq = /* 位置通道 */;
        return any || /* volatile */;
    }
}
```

P0–P2 期间 `_dirtyComponents == 0` 恒成立，fast path 零开销。P3 落地 Component 后自然启用。

### 5.3 动态激活 / 去激活

```csharp
// 脚本
avatar.AddComponent<QuestComponent>();    // 激活 lazy slot
avatar.Combat.Atk = 50;                   // 非 lazy：已自动激活
avatar.RemoveComponent<QuestComponent>(); // 去激活，slot 保留
```

`AddComponent<T>` 逻辑：

1. 在 slot 表查 T 的 slot 索引；未声明 → `Result<Err>` 硬失败
2. 实例化 Component，`_slotIdx = i`，`_entity = this`
3. 调 `OnAttached()`
4. 向本 tick op buffer 写入 `kAddComponent(slotIdx=i, typeId, initialSnapshot)`
5. `_dirtyComponents |= (1UL << i)`

`RemoveComponent` 类似，生成 `kRemoveComponent`，并在本 tick 结束前**丢弃**该 slot 此前的所有 op（Component 已消失，发也没意义）。

### 5.4 两级 scope 过滤（Witness 转发）

```cpp
bool Forward(Op op, ObserverKind who) {
  if (op.component_idx == 0) {
    return ScopeVisibleTo(entity_prop.scope, who);     // Entity 本体
  }
  const auto& comp = type.components[op.component_idx];
  if (!ScopeVisibleTo(comp.scope, who)) return false;  // 总闸
  const auto& prop = comp.properties[op.prop_idx];
  return ScopeVisibleTo(prop.scope, who);              // 分闸
}
```

观察者不具备 component 可见性时，**连 `kAddComponent` 都不下发**；该观察者永远不会收到该 Component 的任何 op，流量 = 0。

### 5.5 Component baseline（新观察者进入 AoI）

基线消息 `kReplicatedBaselineFromCell`（msg 2019）payload：

```
[Entity 本体属性快照]            ← 既有格式
[u8 activeComponentCount]
  loop: [u8 slotIdx] [component snapshot bytes]   ← 按 scope 过滤后
```

**不发** `kAddComponent`：baseline 本身就是"从零装配"。观察者收到后按 slotIdx 直接实例化并填充状态。

### 5.6 Component RPC

路由键：`(entityId, componentIdx, methodIdx)`。

- `componentIdx = 0` → Entity 本体 RPC（现有协议不变）
- `componentIdx > 0` → 对应 Component 方法

Component RPC 的 `exposed` / `cell_methods` / `base_methods` / `client_methods` 语义与现有 Entity RPC 完全一致，仅多一维路由。

**线上布局（P4 bit-pack）**：

```
[componentIdx : bitsRequired(declaredComponentCount + 1) bit]
[methodIdx    : bitsRequired(methodCountOfComponent) bit]
[args ...]
```

两个索引字段**独立编码**（不 flatten）：`componentIdx` 编码宽度由 EntityTypeDescriptor 固化，`methodIdx` 宽度由对应 Component 类型的方法数固化。`componentIdx=0` 时 `methodIdx` 回退成 Entity 本体方法表的索引（与现有协议等价）。比现有 Entity RPC 多 `bitsRequired(declaredCount+1)` 位。

#### 5.6.1 RPC scope 约束（parser 注册期硬校验）

```
method.exposed ⊆ component.scope
```

与属性 `P.scope ⊆ C.scope` 同构：**RPC 的暴露面不得宽于所在 Component 的可见性**。违反则 `DefParser` 在注册期失败。

**示例**：

```xml
<component name="InventoryComponent" scope="own_client">
  <cell_methods>
    <!-- OK: exposed=own_client ⊆ component scope=own_client -->
    <method name="UseItem" exposed="own_client">
      <arg name="itemId" type="int32" />
    </method>

    <!-- ERR: exposed=all_clients ⊄ component scope=own_client
         Component 对非 own 观察者不可见，RPC 暴露给他们没有意义 -->
    <method name="BroadcastLoot" exposed="all_clients">
      <arg name="itemId" type="int32" />
    </method>
  </cell_methods>
</component>
```

#### 5.6.2 运行时 RPC dispatch

```
Client → Server 调用 Component cell_method:
  [entityId][componentIdx][methodIdx][args...]
    │           │             │
    │           │             └── Component 类的方法表索引
    │           └── 静态 slot 索引
    └── 现有字段

CellApp 收到后:
  1. entity = EntityMap[entityId]
  2. if componentIdx == 0: 调 entity 本体方法 (旧路径)
  3. else:
       component = entity._replicated[componentIdx]
       if component == null (slot Declared 但未 Active) → 丢弃 + WARN
       else: component.__Dispatch(methodIdx, args)
  4. exposed 作用域运行时二次校验（防伪造）:
       如果调用方不在 method.exposed 内 → 丢弃 + ERROR
```

Lazy Component（`lazy=true` 但尚未 `AddComponent` 激活）上的 RPC 调用直接丢弃 —— Component 不存在就没有方法表可查，与 `RemoveComponent` 后收到旧 RPC 的处理一致。

---

## 6. 本地 Component（ServerLocal / ClientLocal）

### 6.1 三套独立存储

见 [§4.3](#43-entity-持有三套存储)。本地 component 不参与 `_replicated` 数组，不占 `componentIdx`，不写入 op-log。

### 6.2 源生成器按编译目标分流

Atlas 已分 Server/Client 两个 csproj。源生成器通过 MSBuild 属性感知当前目标：

```xml
<!-- Atlas.Server.csproj -->
<PropertyGroup>
  <AtlasComponentTarget>Server</AtlasComponentTarget>
</PropertyGroup>

<!-- Unity 客户端工程 -->
<PropertyGroup>
  <AtlasComponentTarget>Client</AtlasComponentTarget>
</PropertyGroup>
```

生成器读 `build_property.AtlasComponentTarget`，按 target 过滤产出：

```
Target=Server:
  生成 combat/inventory  (Synced, 继承 ReplicatedComponent)
  生成 ai/lootRoller     (ServerLocal, 继承 ServerLocalComponent)
  跳过 anim/vfx/uiBinder

Target=Client:
  生成 combat/inventory  (Synced, 客户端对偶)
  跳过 ai/lootRoller
  生成 anim/vfx/uiBinder (ClientLocal, 脚手架)
```

Synced component 双端生成的代码在属性字段、Serialize、Apply、MutRef 结构上完全对称，但调用方向不同（服务端写、客户端读），与现有扁平属性的 `SerializeFor*` / `Apply*` 双端生成一脉相承。

### 6.3 两种激活方式并存

**A. 声明式（默认激活）** — `.def` 里声明的本地 component，Entity 构造时自动 `AddLocalComponent<T>()`。

**B. 命令式（动态）** — C# 代码里 `entity.AddLocalComponent<MyCustomComponent>()` 直接挂，不必在 `.def` 声明。

特别适合 Unity 侧按 Prefab 配置差异化挂组件（例：坐骑 Entity 挂 `MountAnimControl`，非坐骑不挂）。

**Synced 只能走 A**（协议要求静态 slot）。本地两条都允许。

### 6.4 Unity 桥接约束

`ClientLocalComponent` 是**纯 POCO**（继承自 `ComponentBase`，非 `MonoBehaviour`），好处：

1. 可在 Entity 生命周期里自由 `new` / GC，不依赖 GameObject
2. 单元测试不需要 PlayMode
3. IL2CPP AOT 干净

需要与 GameObject 交互时通过**桥接接口**：

```csharp
public interface IGameObjectBinding { GameObject? Target { get; } }

public sealed class AnimControlComponent : ClientLocalComponent, IGameObjectBinding
{
    public GameObject? Target { get; private set; }
    private Animator? _animator;

    public override void OnAttached() {
        Target = ClientEntityGameObjectBinder.Find(Entity.EntityId);
        _animator = Target?.GetComponent<Animator>();
    }

    public void Play(string clip) => _animator?.Play(clip);
}
```

`AnimControlComponent` 既可单元测试（不需要 Unity），也可运行时挂到真实 GameObject。

### 6.5 组件间通讯

通过 Entity 做中介：

```csharp
// ClientLocal 订阅 Synced 的属性变更
public sealed class AnimControlComponent : ClientLocalComponent
{
    public override void OnAttached() {
        Entity.Combat.OnAtkChanged += OnAtkChanged;   // 源生成器产出的事件
    }
    public override void OnDetached() {
        Entity.Combat.OnAtkChanged -= OnAtkChanged;
    }
    private void OnAtkChanged(int oldVal, int newVal) { /* play anim */ }
}
```

Component 之间不直接持有引用；统一经 `Entity.GetComponent<T>()` 或具名访问器，避免耦合。

---

## 7. 协议承接点（详细协议见容器同步文档）

本设计对线上协议的影响仅限于**预留字段**，完整协议布局与 op 编码见 [容器属性同步设计 §7 线上协议](./CONTAINER_PROPERTY_SYNC_DESIGN.md#7-线上协议)。

| 预留点 | 布局位置 | P0–P2 取值 | P3 取值 |
|---|---|---|---|
| `componentIdx` | 每条 op header | 恒 0（Entity 本体） | slot 索引 |
| `_dirtyComponents` bitmap | `ServerEntity` 字段 | 恒 0 | slot 脏位 |
| `kAddComponent` / `kRemoveComponent` | `OpKind` 枚举位 | 占位不发射 | 启用 |
| Component RPC 路由 | `(entityId, componentIdx, methodIdx)` | componentIdx=0，等价现有 RPC | 全值路由 |
| Component baseline | `kReplicatedBaselineFromCell` 尾部 | 空 | `[u8 count] + [slotIdx, snapshot]*` |

**协议字段从 P0 起就编码为 `componentIdx` 并占用 `bitsRequired(declaredCount+1)` bit**。P3 启用 Component 时无需版本升级。

---

## 8. 约束与容错

| 场景 | 行为 |
|---|---|
| `AddComponent<T>` T 未在 .def 声明为 Synced | 运行时 `Result<Err>` 硬失败 |
| `AddLocalComponent<T>` T 不继承对应的 `*LocalComponent` | 编译期错误 |
| Lazy slot 未激活时收到该 Component 的 RPC | 丢弃 + WARN |
| 客户端收到 `componentIdx` 超出已知 slot | 丢弃 op，日志 WARN |
| 本地 component 试图在 `.def` 写 `<properties>` | parser 注册期失败 |
| 本地 component slot 名与 Synced slot 名冲突 | parser 注册期失败 |
| 同一 Component 内属性 Cell/Base 位置不一致 | parser 注册期失败 |
| Component RPC `exposed ⊄ scope` | parser 注册期失败 |
| Entity 析构 | 按 slot 倒序调 `OnDetached`，再调本地 component 的 `OnDetached` |

---

## 9. 分阶段实施

| 阶段 | Component 相关范围 |
|---|---|
| **P0** | `componentIdx` 协议字段预留（恒 0）；`_dirtyComponents` 字段预留（恒 0） |
| **P1** | 无额外 Component 动作 |
| **P2** | 无额外 Component 动作 |
| **P3** | Slot 表；`<components>` 段 parser；`ComponentBase` / `ReplicatedComponent` / `ServerLocalComponent` / `ClientLocalComponent` 四基类；`AddComponent`/`RemoveComponent`；两级 scope 过滤；Component RPC；双端源生成器分流；本地 component 两种激活方式 |
| **P4** | 外部线 bit-pack 时，`componentIdx` 按 `bitsRequired(declaredCount+1)` 压缩 |

P3 启动所需的非 Component 前置：P0 的 `ServerEntity._dirtyComponents` / `componentIdx` 协议字段预留已经完成；P1 的 op-log + 可靠通道已经就位；P2 的嵌套容器 op 路径已就位。

---

## 10. 涉及的模块与文件

### C++

| 文件 | 变化 |
|---|---|
| `src/lib/entitydef/entity_type_descriptor.h` | 新增 `ComponentDescriptor`；`EntityTypeDescriptor::components[]` |
| `src/lib/entitydef/component_descriptor.{h,cc}` | 新增（P3） |
| `src/lib/entitydef/entity_def_registry.{h,cc}` | `RegisterComponent`；二进制描述符版本升级 |
| `src/lib/network/message_ids.h` | Component RPC 路由在 P3 加 `componentIdx` 维 |
| `src/server/baseapp/baseapp.cc` | P3 加 Component RPC dispatch |
| `src/server/cellapp/cellapp.cc` | Witness 两级 scope 过滤 |

### C#

| 文件 | 变化 |
|---|---|
| `src/csharp/Atlas.Runtime/Entity/Components/ComponentBase.cs` | 新增（P3） |
| `src/csharp/Atlas.Runtime/Entity/Components/ReplicatedComponent.cs` | 新增（P3） |
| `src/csharp/Atlas.Runtime/Entity/Components/ServerLocalComponent.cs` | 新增（P3，仅服务端工程） |
| `src/csharp/Atlas.Runtime/Entity/Components/ClientLocalComponent.cs` | 新增（P3，仅客户端工程） |
| `src/csharp/Atlas.Runtime/Entity/Components/IGameObjectBinding.cs` | 新增（P3，Unity 桥接接口） |
| `src/csharp/Atlas.Runtime/Entity/ServerEntity.cs` | `_replicated[]` / `_dirtyComponents` / `_serverLocal` / `AddComponent` / `GetComponent` / `AddLocalComponent` |
| `src/csharp/Atlas.Generators.Def/DefParser.cs` | `<components>` 段 + `local` 属性 + 三类 slot 名冲突检测 |
| `src/csharp/Atlas.Generators.Def/Emitters/ComponentEmitter.cs` | 新增（P3，生成 `ReplicatedComponent` / `ServerLocalComponent` / `ClientLocalComponent` 派生类；按 `AtlasComponentTarget` 分流） |
| `src/csharp/Atlas.Generators.Def/Emitters/EntityComponentAccessorEmitter.cs` | 新增（P3，Entity 类上的具名 Component 访问器） |
| `src/csharp/Atlas.Generators.Def/Emitters/DeltaSyncEmitter.cs` | P3 扩展：`BuildAndConsumeReplicationFrame` 汇聚 Component |

---

## 11. 未决项（不阻塞 P0/P1）

- Component 之间的跨 slot 同 tick 顺序保证（倾向：按 slot 索引升序 flush，保证客户端 apply 确定性）
- ServerLocal 在 Entity 迁移（跨 CellApp）时的状态转移（倾向：由用户自行在 `OnAttached`/`OnDetached` 里处理；Synced 走 baseline）
- `AddLocalComponent<T>` 的构造函数注入（是否支持 DI / 构造参数）—— P3 启动前用简单 `new T()` 起步

### 已解决（按拍板时间倒序）

- **2026-04-24**：三分类 Synced / ServerLocal / ClientLocal；`local` 属性；类层级四分；Unity 桥接通过 `IGameObjectBinding`
- **2026-04-24**：Component RPC scope 约束 `method.exposed ⊆ component.scope`
- **2026-04-24**：Component 静态 slot 表；`componentIdx=0` 保留 Entity；`lazy`；两级 scope 过滤
- **2026-04-24**：Component 不嵌套 Component；位置一致性约束
