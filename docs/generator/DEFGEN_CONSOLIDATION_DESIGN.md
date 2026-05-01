# DefGenerator 统一重构设计

> **状态**: 已完成  
> **日期**: 2026-04-14  
> **完成日期**: 2026-04-14  
> **目标**: 以 `.def` 文件作为唯一的实体定义来源，合并 EntityGenerator 和 RpcGenerator 的职责到 DefGenerator，消除 C# Attribute 与 `.def` 的信息冗余

---

## 1. 问题分析

### 1.1 当前状态

Atlas 有 4 个 SourceGenerator，其中 3 个存在功能重叠：

```
.def 文件                    C# Attribute
┌───────────────────┐       ┌──────────────────────────────┐
│ properties:       │       │ [Replicated(AllClients)]      │ ← 重复
│   hp: int32       │       │ [Persistent]                  │ ← 重复
│   scope: all_clients│     │ private int _hp;              │
│   persistent: true │      │                               │
│                    │      │ [CellRpc]                     │ ← 重复
│ cell_methods:      │      │ public partial void CastSkill │
│   CastSkill(int32) │      │                               │
│   exposed: own_client│    │                               │
└────────┬──────────┘       └──────────┬───────────────────┘
         │                             │
   DefGenerator                  RpcGenerator + EntityGenerator
   (RPC stubs, Mailbox,         (RPC stubs, Mailbox,
    Dispatcher, TypeRegistry)    Serialization, DirtyTracking,
                                 DeltaSync, Factory, TypeRegistry)
```

**冗余点**:

| 信息 | `.def` 中定义 | C# Attribute 定义 | 重复？ |
|------|-------------|-------------------|--------|
| RPC 方法签名 | `<method name="CastSkill">` | `[CellRpc] partial void CastSkill(...)` | 是 |
| RPC 方向 | `<cell_methods>` 节 | `[CellRpc]` / `[BaseRpc]` / `[ClientRpc]` | 是 |
| RPC 暴露范围 | `exposed="own_client"` | 无对应 (RpcGenerator 没有此功能) | .def 更完整 |
| 属性类型 | `type="int32"` | 字段声明 `private int _hp` | 是 |
| 复制范围 | `scope="all_clients"` | `[Replicated(Scope=AllClients)]` | 是 |
| 持久化 | `persistent="true"` | `[Persistent]` | 是 |
| 详细等级 | `detail_level="3"` | `[Replicated(DetailLevel=3)]` | 是 |
| 实体名称 | `<entity name="Avatar">` | `[Entity("Avatar")]` | 桥接用，保留 |

### 1.2 重构前 Generator 矩阵

> **注意**: 以下为重构前的状态，EntityGenerator 和 RpcGenerator 已在重构中删除。

| Generator | 输入 | 输出 | 问题 |
|-----------|------|------|------|
| **DefGenerator** | .def + `[Entity]` | RPC stubs, Mailbox, RpcIds, Dispatcher, TypeRegistry | 不处理属性序列化 |
| ~~EntityGenerator~~ | `[Entity]` + `[Replicated]` + `[Persistent]` | Serialization, DirtyTracking, DeltaSync, Factory, TypeRegistry | 与 .def 信息重复，**已删除** |
| ~~RpcGenerator~~ | `[Entity]` + `[*Rpc]` | RPC stubs, Mailbox, RpcIds, Dispatcher | 与 DefGenerator 完全重叠，**已删除** |

### 1.3 已解决的冲突

- **RpcIds.g.cs**: ~~DefGenerator 和 RpcGenerator 各自生成一份~~ → 现在仅 DefGenerator 生成
- **Dispatcher**: ~~两套并存~~ → 现在仅 DefGenerator 生成 `DefRpcDispatcher.g.cs`
- **TypeRegistry**: ~~EntityGenerator 缺少 RPC 信息~~ → 现在由 DefGenerator 的 `DefEntityTypeRegistry` 统一生成，含完整属性和 RPC 信息
- **类型映射**: ~~四份几乎相同的代码~~ → 仅保留 `DefTypeHelper`

---

## 2. 目标架构

### 2.1 合并后的 Generator 矩阵

| Generator | 输入 | 输出 | 状态 |
|-----------|------|------|------|
| **DefGenerator** (合并后) | `.def` + `[Entity]` | **全部**: RPC stubs, Mailbox, RpcIds, Dispatcher, TypeRegistry, **Properties, Serialization, DirtyTracking, DeltaSync, Factory** | 唯一实体 Generator, **已完成** |
| ~~EntityGenerator~~ | - | - | **已删除** |
| ~~RpcGenerator~~ | - | - | **已删除** |
| ~~EventGenerator~~ | - | - | **已删除** |

### 2.2 单一数据流

```
Avatar.def (唯一定义)               Avatar.cs (纯业务逻辑)
┌─────────────────────────┐        ┌──────────────────────────┐
│ <entity name="Avatar">  │        │ [Entity("Avatar")]       │
│   <properties>           │        │ public partial class Avatar │
│     <property name="hp"  │        │ {                        │
│       type="int32"       │        │   public partial void    │
│       scope="all_clients"│        │     CastSkill(int id)    │
│       persistent="true"/>│        │   {                      │
│   </properties>          │        │     Hp -= 50;            │
│   <cell_methods>         │        │   }                      │
│     <method name=        │        │                          │
│       "CastSkill"        │        │   // Hp 属性由 Generator │
│       exposed="own_client">│      │   // 自动生成, 包含      │
│       <arg name="id"     │        │   // dirty tracking      │
│         type="int32"/>   │        │ }                        │
│     </method>            │        └──────────────────────────┘
│   </cell_methods>        │
│   <client_methods>       │
│     <method name=        │
│       "ShowDamage">      │
│       <arg name="amount" │
│         type="int32"/>   │
│     </method>            │
│   </client_methods>      │
│ </entity>                │
└────────────┬─────────────┘
             │
       DefGenerator (统一)
             │
             ├── Avatar.RpcStubs.g.cs        (现有)
             ├── Avatar.Mailboxes.g.cs       (现有)
             ├── Avatar.Properties.g.cs      (新增: 字段 + Property + DirtyTracking)
             ├── Avatar.Serialization.g.cs   (移入: 序列化/反序列化)
             ├── Avatar.DeltaSync.g.cs       (移入: Owner/Other 同步)
             ├── RpcIds.g.cs                 (现有)
             ├── DefRpcDispatcher.g.cs       (现有)
             ├── EntityFactory.g.cs          (移入)
             └── DefEntityTypeRegistry.g.cs  (现有, 增强: 含完整 RPC + 属性信息)
```

### 2.3 C# Attribute 变更

| Attribute | 变更 | 理由 |
|-----------|------|------|
| `[Entity("Name")]` | **保留** | 关联 C# class 与 .def 文件的桥梁 |
| `[Entity].Compression` | **待移到 .def** | 在 .def 中增加 `compression="deflate"` 属性（未实现） |
| ~~`[Replicated]`~~ | **已删除** | `.def` 的 `<property scope="...">` 替代 |
| ~~`[Persistent]`~~ | **已删除** | `.def` 的 `persistent="true"` 替代 |
| ~~`[ServerOnly]`~~ | **已删除** | `.def` 中不定义 scope 或 scope="cell_private" 替代 |
| ~~`[ClientRpc]`~~ | **已删除** | `.def` 的 `<client_methods>` 替代 |
| ~~`[CellRpc]`~~ | **已删除** | `.def` 的 `<cell_methods>` 替代 |
| ~~`[BaseRpc]`~~ | **已删除** | `.def` 的 `<base_methods>` 替代 |
| ~~`[EventHandler]`~~ | **已删除** | EventBus 与 EventGenerator 已整体移除（无业务调用方） |

> **注意**: 原计划是先标记 `[Obsolete]` 再删除。实际实施中因无外部消费者，直接删除了所有废弃属性及其关联的 `ReplicationScope` 枚举。

---

## 3. 新增生成内容

### 3.1 Properties 生成 (Avatar.Properties.g.cs)

DefGenerator 从 `.def` 的 `<properties>` 节读取属性定义，生成字段声明、Property 和 dirty tracking。

**输入** (.def):
```xml
<property name="hp" type="int32" scope="all_clients" persistent="true" />
<property name="name" type="string" scope="own_client" persistent="true" />
<property name="speed" type="float" scope="all_clients" />
```

**输出** (Avatar.Properties.g.cs):
```csharp
// <auto-generated />
partial class Avatar
{
    // ---- 字段 (private, 由 .def 定义) ----
    private int _hp;
    private string _name = "";
    private float _speed;

    // ---- Property (public, 含 dirty tracking) ----
    public int Hp
    {
        get => _hp;
        set
        {
            if (_hp != value)
            {
                _hp = value;
                _dirtyFlags |= DirtyFlags.Hp;
            }
        }
    }

    public string Name
    {
        get => _name;
        set
        {
            if (_name != value)
            {
                _name = value;
                _dirtyFlags |= DirtyFlags.Name;
            }
        }
    }

    public float Speed
    {
        get => _speed;
        set
        {
            if (_speed != value)
            {
                _speed = value;
                _dirtyFlags |= DirtyFlags.Speed;
            }
        }
    }

    // ---- Dirty Flags ----
    [Flags]
    private enum DirtyFlags : byte  // byte/ushort/uint/ulong 根据字段数选择
    {
        None  = 0,
        Hp    = 1 << 0,
        Name  = 1 << 1,
        Speed = 1 << 2,
    }

    private DirtyFlags _dirtyFlags;
    public bool IsDirty => _dirtyFlags != DirtyFlags.None;
    public void ClearDirty() => _dirtyFlags = DirtyFlags.None;
}
```

**字段命名规则**: `.def` 中 `name="hp"` → 字段 `_hp`，Property `Hp`（首字母大写）。

**属性变更回调**: 提供 partial method 钩子让用户实现自定义逻辑：

```csharp
// 生成
partial void OnHpChanged(int oldValue, int newValue);

public int Hp
{
    get => _hp;
    set
    {
        if (_hp != value)
        {
            var old = _hp;
            _hp = value;
            _dirtyFlags |= DirtyFlags.Hp;
            OnHpChanged(old, value);
        }
    }
}
```

### 3.2 Serialization 生成 (Avatar.Serialization.g.cs)

从 `.def` properties 生成全量序列化/反序列化，原来由 EntityGenerator 的 SerializationEmitter 完成。

**输出**:
```csharp
partial class Avatar
{
    public void Serialize(ref SpanWriter writer)
    {
        writer.WriteByte(1);                    // version
        writer.WriteUInt16(3);                  // field count
        var bodyPos = writer.ReserveUInt16();    // body length (backpatch)
        writer.WriteInt32(_hp);
        writer.WriteString(_name);
        writer.WriteFloat(_speed);
        writer.BackpatchUInt16(bodyPos);
    }

    public void Deserialize(ref SpanReader reader)
    {
        var version = reader.ReadByte();
        var fieldCount = reader.ReadUInt16();
        var bodyLength = reader.ReadUInt16();
        if (fieldCount > 0) _hp = reader.ReadInt32();
        if (fieldCount > 1) _name = reader.ReadString();
        if (fieldCount > 2) _speed = reader.ReadFloat();
        // 跳过未知字段 (版本前向兼容)
    }
}
```

### 3.3 DeltaSync 生成 (Avatar.DeltaSync.g.cs)

从 `.def` properties 的 scope 生成分范围同步代码。

**输出**:
```csharp
partial class Avatar
{
    /// 序列化增量 (仅脏字段)
    public void SerializeReplicatedDelta(ref SpanWriter writer)
    {
        writer.WriteByte((byte)_dirtyFlags);
        if ((_dirtyFlags & DirtyFlags.Hp) != 0) writer.WriteInt32(_hp);
        if ((_dirtyFlags & DirtyFlags.Name) != 0) writer.WriteString(_name);
        if ((_dirtyFlags & DirtyFlags.Speed) != 0) writer.WriteFloat(_speed);
    }

    /// 应用增量
    public void ApplyReplicatedDelta(ref SpanReader reader)
    {
        var flags = (DirtyFlags)reader.ReadByte();
        if ((flags & DirtyFlags.Hp) != 0) _hp = reader.ReadInt32();
        if ((flags & DirtyFlags.Name) != 0) _name = reader.ReadString();
        if ((flags & DirtyFlags.Speed) != 0) _speed = reader.ReadFloat();
    }

    /// Owner 客户端全量同步 (scope >= OwnClient)
    public void SerializeForOwnerClient(ref SpanWriter writer)
    {
        writer.WriteInt32(_hp);      // scope: all_clients
        writer.WriteString(_name);   // scope: own_client
        writer.WriteFloat(_speed);   // scope: all_clients
    }

    /// 其他客户端全量同步 (scope >= AllClients)
    public void SerializeForOtherClients(ref SpanWriter writer)
    {
        writer.WriteInt32(_hp);      // scope: all_clients
        // _name 跳过: scope=own_client, 其他客户端不可见
        writer.WriteFloat(_speed);   // scope: all_clients
    }
}
```

### 3.4 EntityFactory 生成 (EntityFactory.g.cs)

从所有 `.def` 文件和 `[Entity]` 标记的类生成工厂，原来由 EntityGenerator 的 FactoryEmitter 完成。

---

## 4. .def Schema 扩展

### 4.1 新增属性

```xml
<entity name="Avatar" compression="deflate">
  <!-- compression: none(默认) | deflate -->

  <properties>
    <property name="hp"
              type="int32"
              scope="all_clients"
              persistent="true"
              detail_level="0"
              default="100" />  <!-- 新增: 默认值 -->
  </properties>

  <!-- RPC 部分不变 -->
</entity>
```

| 新增字段 | 说明 |
|----------|------|
| `compression` | 实体级压缩设置，替代 `[Entity(Compression=1)]` |
| `default` | 属性默认值，生成字段初始化表达式 |

### 4.2 类型映射 (不变)

| .def 类型 | C# 类型 | SpanWriter 方法 | DataType ID |
|-----------|---------|----------------|-------------|
| bool | bool | WriteBool | 0 |
| int8 | sbyte | WriteSByte | 1 |
| uint8 | byte | WriteByte | 2 |
| int16 | short | WriteInt16 | 3 |
| uint16 | ushort | WriteUInt16 | 4 |
| int32 | int | WriteInt32 | 5 |
| uint32 | uint | WriteUInt32 | 6 |
| int64 | long | WriteInt64 | 7 |
| uint64 | ulong | WriteUInt64 | 8 |
| float | float | WriteFloat | 9 |
| double | double | WriteDouble | 10 |
| string | string | WriteString | 11 |
| bytes | byte[] | WriteBytes | 12 |
| vector3 | Vector3 | WriteVector3 | 13 |
| quaternion | Quaternion | WriteQuaternion | 14 |

---

## 5. 生成文件清单 (合并后)

### 5.1 每个实体生成

| 文件 | 内容 | 原来源 |
|------|------|--------|
| `{Class}.Properties.g.cs` | 字段声明、Property、DirtyFlags、变更回调 | **新增** (部分来自 EntityGenerator.DirtyTracking) |
| `{Class}.Serialization.g.cs` | Serialize/Deserialize 全量 | EntityGenerator.Serialization |
| `{Class}.DeltaSync.g.cs` | Delta/Owner/Other 同步 | EntityGenerator.DeltaSync |
| `{Class}.RpcStubs.g.cs` | RPC Send/Receive/Forbidden 方法 | DefGenerator (现有) |
| `{Class}.Mailboxes.g.cs` | Mailbox 结构体和属性 | DefGenerator (现有) |

### 5.2 全局文件

| 文件 | 内容 | 原来源 |
|------|------|--------|
| `RpcIds.g.cs` | RPC ID 常量 | DefGenerator (现有) |
| `DefRpcDispatcher.g.cs` | RPC 分发 switch | DefGenerator (现有) |
| `EntityFactory.g.cs` | 实体工厂 | EntityGenerator.Factory |
| `DefEntityTypeRegistry.g.cs` | 二进制类型描述符 | DefGenerator (现有, 已含 RPC + 属性) |

---

## 6. 清理结果

### 6.1 已删除的 Attribute

以下 Attribute 和类型已从 `Atlas.Shared/Entity/Attributes.cs` 和 `Atlas.Shared/Rpc/RpcAttributes.cs` 中删除：

| 已删除项 | 原文件 | 替代方案 |
|----------|--------|----------|
| `ReplicatedAttribute` | `Attributes.cs` | `.def` 的 `<property scope="...">` |
| `PersistentAttribute` | `Attributes.cs` | `.def` 的 `persistent="true"` |
| `ServerOnlyAttribute` | `Attributes.cs` | `.def` 的 `scope="cell_private"` |
| `ReplicationScope` 枚举 | `Attributes.cs` | `.def` 的 8 值 PropertyScope |
| `ClientRpcAttribute` | `RpcAttributes.cs` (已删除) | `.def` 的 `<client_methods>` |
| `CellRpcAttribute` | `RpcAttributes.cs` (已删除) | `.def` 的 `<cell_methods>` |
| `BaseRpcAttribute` | `RpcAttributes.cs` (已删除) | `.def` 的 `<base_methods>` |

### 6.2 已删除的 Generator 项目

| 项目 | 处理 |
|------|------|
| `Atlas.Generators.Entity` | 功能合并入 DefGenerator，整个目录已删除 |
| `Atlas.Generators.Rpc` | DefGenerator 已覆盖，整个目录已删除 |

### 6.3 保留的项目

| 项目 | 理由 |
|------|------|
| `Atlas.Generators.Def` | 合并后的唯一实体 Generator |

---

## 7. 类型辅助代码统一

### 7.1 当前状况

合并前四个 Generator 各有一份几乎相同的类型映射代码：

- `DefTypeHelper.cs` — .def 类型 → C# 类型 / Writer/Reader 方法 / DataType ID
- `EntityTypeHelper.cs` — C# 类型 → Writer/Reader 方法 / DataType ID
- `RpcTypeHelper.cs` — C# 类型 → Writer/Reader 方法 / DataType ID + 大小估算

合并完成后仅保留 `DefTypeHelper`，无需进一步引入 `Atlas.Generators.Common`
共享项目（仅一个 Generator，没有共享需求）。

---

## 8. 上下文感知生成

### 8.1 ProcessContext (不变)

DefGenerator 根据编译符号决定生成上下文：

| 编译符号 | 上下文 | 角色 |
|----------|--------|------|
| `ATLAS_BASE` | Base 进程 | base_methods → Receive, cell/client_methods → Send |
| `ATLAS_CELL` | Cell 进程 | cell_methods → Receive, base/client_methods → Send |
| `ATLAS_CLIENT` | 客户端 | client_methods → Receive, exposed cell/base_methods → Send |
| 无 | Server 通用 | 所有方向都接收 |

### 8.2 属性生成的上下文差异

新增的属性序列化/Delta 生成也需要区分上下文：

| 生成内容 | Base | Cell | Client |
|----------|------|------|--------|
| 字段声明 + Property | 所有字段 | 所有字段 | scope ≥ OwnClient 的字段 |
| Serialize (全量) | 所有字段 | 所有字段 | 不生成 (客户端不发全量) |
| Deserialize (全量) | 所有字段 | 所有字段 | scope ≥ OwnClient 的字段 |
| SerializeReplicatedDelta | scope ≥ OwnClient | scope ≥ OwnClient | 不生成 |
| ApplyReplicatedDelta | 所有字段 | 所有字段 | scope ≥ OwnClient 的字段 |
| SerializeForOwnerClient | 生成 | 生成 | 不生成 |
| SerializeForOtherClients | 生成 | 生成 | 不生成 |
| DirtyFlags | 可复制字段 | 可复制字段 | 不生成 (客户端不追踪 dirty) |

**客户端只需要**: 字段声明、Property（只读或带本地预测标记）、ApplyReplicatedDelta。

---

## 9. 用户代码变更对照

### 9.1 重构前

```csharp
// Avatar.def
// <entity name="Avatar">
//   <properties>
//     <property name="hp" type="int32" scope="all_clients" persistent="true" />
//   </properties>
//   <cell_methods>
//     <method name="CastSkill" exposed="own_client">
//       <arg name="skillId" type="int32" />
//     </method>
//   </cell_methods>
// </entity>

// Avatar.cs — 用户需要重复声明字段和标记
[Entity("Avatar")]
public partial class Avatar : CellEntity
{
    [Replicated(Scope = ReplicationScope.AllClients)]   // ← 与 .def 重复
    [Persistent]                                         // ← 与 .def 重复
    private int _hp;

    public int Hp { get => _hp; set => _hp = value; }   // ← 手写 Property

    [CellRpc]                                            // ← 与 .def 重复
    public partial void CastSkill(int skillId);

    // 实现
    public partial void CastSkill(int skillId)
    {
        Hp -= 50;
    }
}
```

### 9.2 重构后

```csharp
// Avatar.def — 唯一定义 (不变)

// Avatar.cs — 只写业务逻辑
[Entity("Avatar")]
public partial class Avatar : CellEntity
{
    // Hp 属性由 DefGenerator 自动生成, 包含:
    //   private int _hp;
    //   public int Hp { get; set; }  (含 dirty tracking)

    // CastSkill 的 RPC stub 由 DefGenerator 生成
    // 用户只需实现 partial method
    public partial void CastSkill(int skillId)
    {
        Hp -= 50;
    }

    // 可选: 属性变更回调
    partial void OnHpChanged(int oldValue, int newValue)
    {
        if (newValue <= 0) Die();
    }
}
```

**用户代码减少**: 去掉了所有 Attribute 标注和字段声明，只保留业务逻辑。

---

## 10. 实施步骤

> **实际执行记录**: Phase 1 跳过（未提取 Common 项目），Phase 2 完成，Phase 3 待后续，Phase 4+5 合并执行。

### Phase 1: 共享代码提取 — 跳过

原计划创建 `Atlas.Generators.Common` 项目；EventGenerator 后续整体移除后，仅剩 DefGenerator，无共享需求。

### Phase 2: DefGenerator 增加属性生成 — **已完成**

1. ✅ 新增 `PropertiesEmitter` — 字段、Property、DirtyFlags、`On{Name}Changed` 回调
2. ✅ 新增 `SerializationEmitter` — Serialize/Deserialize（version + fieldCount + bodyLength 格式）
3. ✅ 新增 `DeltaSyncEmitter` — SerializeReplicatedDelta/ApplyReplicatedDelta/Owner/Other
4. ✅ 新增 `FactoryEmitter` — EntityFactory（Dictionary 查找，static ctor 填充）
5. ✅ `TypeRegistryEmitter` 增加 try-catch 保护（DllNotFoundException），测试环境可用
6. ✅ 所有 Emitter 支持 ProcessContext 上下文感知
7. ✅ `DefTypeHelper` 新增 `ToPropertyName`/`ToFieldName`/`DefaultValue`/`NeedsFieldInitializer`
8. ✅ Vector3/Quaternion 类型正确映射到 `Atlas.DataTypes` 而非 `System.Numerics`
9. ✅ Client 上下文 Deserialize 对不可见字段读取后丢弃（`_ = reader.Read...()`）

### Phase 3: .def Schema 扩展 — 待后续

1. DefParser 支持 `compression` 实体属性
2. DefParser 支持 `default` 属性默认值
3. 更新 .def 文件 schema 文档

### Phase 4+5: 删除旧 Generator 和属性 — **已完成**

原计划分两阶段（先废弃后删除），实际合并执行（无外部消费者，无需过渡期）：

1. ✅ 删除 `Atlas.Generators.Entity` 项目（整个目录）
2. ✅ 删除 `Atlas.Generators.Rpc` 项目（整个目录）
3. ✅ 删除 `ReplicatedAttribute`、`PersistentAttribute`、`ServerOnlyAttribute`、`ReplicationScope`
4. ✅ 删除 `RpcAttributes.cs`（`ClientRpcAttribute`、`CellRpcAttribute`、`BaseRpcAttribute`）
5. ✅ 从 `Atlas.Runtime.csproj` 移除 EntityGenerator 和 RpcGenerator 的 Analyzer 引用
6. ✅ 测试实体改用 `.def` 文件，测试 `.csproj` 切换到 DefGenerator
7. ✅ Sample 实体移除手写的 `TypeName`/`Serialize`/`Deserialize` 桩
8. ✅ `ClientEntity` 添加 `virtual void Deserialize(ref SpanReader)` 方法
9. ✅ `EngineContext` 移除 `EntityTypeRegistry.RegisterAll()` 调用（由 `[ModuleInitializer]` 自动处理）
10. ✅ 创建默认空 `EntityFactory`（Runtime 中，被 DefGenerator 生成版本通过 CS0436 覆盖）
11. ✅ 所有 67 个测试通过（51 Runtime + 16 Generator），所有项目编译通过

---

## 11. 诊断代码 (合并后)

| 代码 | 级别 | 说明 |
|------|------|------|
| DEF001 | Error | `[Entity]` 类没有匹配的 .def 文件 |
| DEF002 | Error | client_methods 不能有 exposed 属性 |
| DEF003 | Error | base_methods 不能使用 exposed="all_clients" |
| DEF004 | Error | 不支持的属性类型 |
| DEF005 | Error | `[Entity]` 类必须是 partial |
| DEF006 | Error | .def XML 解析错误 |
| DEF007 | Error | 重复的 type_id |
| ~~DEF008~~ | ~~Warning~~ | ~~使用了废弃的 `[Replicated]` 属性~~ — 属性已删除，不再需要 |
| ~~DEF009~~ | ~~Warning~~ | ~~使用了废弃的 `[*Rpc]` 属性~~ — 属性已删除，不再需要 |
| DEF010 | Error | .def 属性与 C# 已有字段名冲突 |
| DEF011 | Error | `[Entity]` 类必须继承正确的基类 |

---

## 12. 迁移风险与缓解

| 风险 | 影响 | 缓解措施 | 结果 |
|------|------|----------|------|
| 现有项目依赖旧 Attribute | 编译错误 | ~~Phase 4 先标 Obsolete~~ 直接删除 | ✅ 无外部消费者，无影响 |
| 字段名冲突 | 生成的字段与用户手写字段重名 | DEF010 诊断检测冲突 | ✅ 待实现 |
| Property setter 自定义逻辑丢失 | 行为变更 | 提供 `partial void On{Name}Changed()` 回调 | ✅ 已实现 |
| 序列化格式变更 | 数据不兼容 | 保持 version/fieldCount/bodyLength 格式 | ✅ 格式兼容 |
| Sample 实体手写 stub 冲突 | 重复定义 | 移除手写 TypeName/Serialize/Deserialize | ✅ 已清理 |
| ClientEntity 缺少 Deserialize | 客户端无法反序列化 | ClientEntity 添加 virtual Deserialize | ✅ 已添加 |
