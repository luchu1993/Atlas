# Component 体系

> 关联：[property_sync_design.md](./property_sync_design.md) · [container_property_sync_design.md](./container_property_sync_design.md) · [BigWorld RPC 参考](../bigworld_ref/BIGWORLD_RPC_REFERENCE.md)

Atlas Entity 用 **三类 Component** 组织模块化逻辑：

| 分类 | 来源 | 参与协议 | 占 `slot_idx` | 双端 | 典型用途 |
|---|---|---|---|---|---|
| **Synced** | `<components>` 默认 / 独立 `<component>` def | ✓ | ✓ | ✓ | Combat、Inventory、Quest |
| **ServerLocal** | `local="server"` / 运行时 `AddLocalComponent<T>()` | ✗ | ✗ | 仅服务端 | ServerAI、LootRoller、AntiCheat |
| **ClientLocal** | `local="client"` / 运行时 `AddLocalComponent<T>()` | ✗ | ✗ | 仅客户端 | AnimControl、VFX、UIBinder、InputPredictor |

只有 Synced 走 `slot_idx` 协议字段；本地组件**不污染** slot 表，否则编码宽度漂移、双端不一致。

---

## 1. .def 声明

```xml
<entity name="Avatar">
  <components>
    <!-- Synced：默认，参与 slot 表 -->
    <component name="combat"    type="CombatComponent" />
    <component name="inventory" type="InventoryComponent" scope="own_client" />
    <component name="quest"     type="QuestComponent"     lazy="true" />

    <!-- ServerLocal：仅服务端工程实例化 -->
    <component name="ai"         type="ServerAIComponent"   local="server" />

    <!-- ClientLocal：仅客户端工程实例化 -->
    <component name="anim"     type="AnimControlComponent" local="client" />
    <component name="uiBinder" type="UIBinderComponent"    local="client" />
  </components>
</entity>
```

`name` 是 slot 名（C# 访问器属性名），`type` 是组件类型名。槽与类名都必须实体内唯一（DEF015）。

### 1.1 独立 `<component>` def 文件 + 单继承

```xml
<component name="StressLoadComponent" extends="LoadComponentBase">
  <properties>
    <property name="weight" type="int32" scope="all_clients" />
  </properties>
  <client_methods> ... </client_methods>
  <cell_methods> ... </cell_methods>
</component>
```

`DefLinker` 跨文件解析 `extends` 链；`propIdx` 在层级中扁平化（派生组件第一条属性从 `BaseProps.Count` 起）。

### 1.2 约束（DefParser / DefLinker 静态校验）

- 属性 `P.scope ⊆ C.scope`（observer 子集关系，DEF006）—— 不允许把 `all_clients` 属性塞进 `own_client` 组件
- `local="server"` / `local="client"` 组件**禁止** `<properties>` / `<client_methods>` / `<cell_methods>` / `<base_methods>`：本地状态与方法直接写在 C# 类
- Component 不能嵌套 Component；要组合多份数据用 `list[struct]`
- RPC 同样遵守 `client_methods` 不能 `exposed`（DEF002）、`base_methods` 不能 `exposed="all_clients"`（DEF003）

---

## 2. 类层级（双端对称）

```
ComponentBase                            Atlas.Entity.Components / Atlas.Components
├── ReplicatedComponent (服务端)         继承者由 generator 产出，落在 Atlas.Components 命名空间
├── ServerLocalComponent (服务端)
└── ClientReplicatedComponent (客户端)   ClientLocalComponent (客户端)
```

服务端基类在 `src/csharp/Atlas.Runtime/Entity/Components/`，客户端在 `src/csharp/Atlas.Client/Components/`。Synced 组件由 `ComponentEmitter` 产出双端各一份对称的 `partial class`，落在 `Atlas.Components` 命名空间。

### 2.1 共享生命周期（`ComponentBase`）

```csharp
public virtual void OnAttached() { }
public virtual void OnDetached() { }
public virtual void OnTick(float deltaTime) { }
```

- `OnAttached` —— Entity 创建 / `AddComponent<T>()` / `AddLocalComponent<T>()` 时同步调
- `OnDetached` —— Entity 析构（按 slot 倒序，先 Synced 后 Local）/ `RemoveComponent<T>()` 同步调
- `OnTick(dt)` —— 服务端由 `entity.TickAllComponents(dt)` 在 entity 自身 `OnTick` 之后调；先按 slot 升序遍历 Active Synced，再按挂载顺序遍历 `_serverLocal`。客户端 `ClientEntity.TickAllComponents` 同形。

### 2.2 服务端 Synced 专属

`ReplicatedComponent` 持 per-component `_dirtyFlags`（ulong，最多 64 个属性）+ `_slotIdx`：

```csharp
protected void MarkDirty(int propIdx)
{
    _dirtyFlags |= 1UL << propIdx;
    _entity.__MarkComponentDirty(_slotIdx);    // 让 entity 的 _dirtyComponents 也亮位
}

public virtual bool BuildFrame(...)               // generator override
public virtual bool HasOwnerDirty()               // sectionMask bit2 决策
public virtual bool HasOtherDirty()
public virtual void WriteOwnerDelta(...)
public virtual void WriteOtherDelta(...)
```

两级 dirty（per-prop 在组件内 + per-component 在实体内）让 `BuildAndConsumeReplicationFrame` 能在 `_dirtyComponents == 0` 时跳过整个组件遍历。

### 2.3 RPC `rpc_id` 编码

每个组件方法的 `rpc_id`（generator 在 `ReplicatedComponent.SendXxxRpc` / 客户端对偶处合成）：

```
[ slot_idx (8 bit) ][ direction (2 bit) ][ TypeId (8 bit) ][ methodIdx (剩余) ]
```

- `slot_idx == 0` 等价 entity 本体方法
- `direction`：00=Client / 01=保留 / 10=Cell / 11=Base
- `TypeId` 让单一组件类可以挂在不同 entity 类型上各走各的 dispatcher

---

## 3. 实体侧持有

### 3.1 `ServerEntity` / `ClientEntity` 结构

```csharp
public abstract class ServerEntity        // 客户端 ClientEntity 同形
{
    public ReplicatedComponent?[]? _replicated;     // 静态 slot 表，索引 = slot_idx
    private protected ulong _dirtyComponents;
    private Dictionary<Type, ServerLocalComponent>? _serverLocal;

    public T AddComponent<T>() where T : ReplicatedComponent, new();   // Synced：查 slot
    public T? GetSyncedComponent<T>() where T : ReplicatedComponent;
    public bool RemoveComponent<T>() where T : ReplicatedComponent;

    public T AddLocalComponent<T>() where T : ServerLocalComponent, new();   // 不占 slot
    public T? GetLocalComponent<T>() where T : ServerLocalComponent;
    public bool RemoveLocalComponent<T>() where T : ServerLocalComponent;

    protected virtual int SyncedSlotCount => 0;                     // generator override
    protected virtual int ResolveSyncedSlot(Type componentType) => -1;
}
```

`_replicated[0]` 永久保留给 entity 本体，用户 slot 从 1 起。

### 3.2 强类型访问器（generator 产出）

`EntityComponentAccessorEmitter` 为每个声明 `<components>` 的实体产出具名访问器：

```csharp
public partial class StressAvatar
{
    public StressLoadComponent? Load => GetSyncedComponent<StressLoadComponent>();
    public StressTimerComponent? Timer => GetLocalComponent<StressTimerComponent>();   // ServerLocal
    // 客户端：
    public StressMetricsComponent? Metrics => GetLocalComponent<StressMetricsComponent>();
}
```

脚本写 `avatar.Load.Weight = 50`，O(1) slot 索引、零反射、零装箱。

### 3.3 服务端 `BuildAndConsumeReplicationFrame` 整合

`ServerEntity` 帧末入口（generator override）：

```csharp
bool hasEvent = (_dirtyFlags & ...) != None ||
                HasOwnerDirtyComponent() || HasOtherDirtyComponent();
if (hasEvent) {
    SerializeOwnerDelta(ref ownerDelta);   // 内部按 sectionMask bit2 串接 component section
    SerializeOtherDelta(ref otherDelta);
    _dirtyFlags = None;
    ClearDirtyComponents();
}
```

Component section 在 sectionMask 上占 bit2，wire 布局见 [property_sync_design.md §2.2](./property_sync_design.md#22-sectionmask-字节布局)。

---

## 4. 本地组件（ServerLocal / ClientLocal）

### 4.1 用法

声明式（`.def` 标 `local="..."`）—— Entity 构造时由 generator 自动 `AddLocalComponent<T>()`。
命令式 —— C# 代码里 `entity.AddLocalComponent<MyCustomComponent>()` 直接挂。

Synced 只能走声明式（协议要求静态 slot）；本地两条都允许。

### 4.2 Unity 桥接

`ClientLocalComponent` 是**纯 POCO**，不继承 `MonoBehaviour`。需要 GameObject 时通过桥接接口（推荐）：

```csharp
public interface IGameObjectBinding { GameObject? Target { get; } }

public sealed class AnimControlComponent : ClientLocalComponent, IGameObjectBinding
{
    public GameObject? Target { get; private set; }
    public override void OnAttached()
    {
        Target = ClientEntityGameObjectBinder.Find(Entity.EntityId);
        // attach Animator / VFX / etc.
    }
}
```

收益：可在 Entity 生命周期里自由 new / GC、单元测试不需要 PlayMode、IL2CPP AOT 干净。

### 4.3 组件间通讯

通过 Entity 中介，不直接持有彼此引用：

```csharp
public sealed class AnimControlComponent : ClientLocalComponent
{
    public override void OnAttached()
    {
        // generator-emitted change callback partial 可被 user partial override
        // 或脚本侧轮询 Entity.Combat.Atk
    }
}
```

---

## 5. 协议预留：尚未启用的 op

`Atlas.Observable.OpKind` 枚举里 `AddComponent = 6` / `RemoveComponent = 7` 仍保留为占位 —— 当前组件的 enable / disable 不通过 op-log 投递，而由 entity baseline 与 `_dirtyComponents` 位图静态承载。运行时动态 `AddComponent<QuestComponent>()` 已可用，新观察者进入 AoI 时通过 baseline `kReplicatedBaselineFromCell` 一次性同步所有 Active Synced 组件状态。Lazy slot 在被 RemoveComponent 后会丢弃残留 op（见 §2.2 `ClearDirtyComponents`）。

P3 后续若需要"add/remove 在 tick 边界精确事件化"（避免 baseline 抖动），再启用这两个 op kind。

---

## 6. 容错

| 场景 | 行为 |
|---|---|
| `AddComponent<T>()` T 未在 `.def` 声明为 Synced | `InvalidOperationException`（`ResolveSyncedSlot` 返回 -1） |
| `AddLocalComponent<T>()` T 不继承对应 `*LocalComponent` | 编译期 `where` 约束错误 |
| Lazy slot 未激活时收到该组件的 RPC | dispatcher 见 `_replicated[i] == null` 丢弃并 WARN |
| 客户端收到 `slotIdx` 超出已知 slot | sectionMask bit2 段解码越界，触发 `IsCorrupted` 标记 |
| `local="server"` 组件声明 `<properties>` | DefParser 报 DEF006 |
| 同一组件属性 Cell/Base 位置不一致 | 实际由 `PropertyScope.IsBase()`/`IsCell()` 互斥保证；不可能跨进程 |
| Component RPC `exposed ⊄ scope` | DefLinker 在校验阶段拒绝（见 §1.2） |
| Entity 析构 | 按 slot 倒序调 Synced 的 `OnDetached`，再调 `_serverLocal` / `_clientLocal` 的 `OnDetached` |

