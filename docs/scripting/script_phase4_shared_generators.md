# ScriptPhase 4: 共享程序集 + Source Generator 体系

> 预估周期: 3–4 周 | 前置依赖: ScriptPhase 3 完成

---

## 目标

1. 建立 `Atlas.Shared` 共享程序集 (netstandard2.1)，可在服务端和 Unity IL2CPP 上运行
2. 开发 Entity / RPC / Events 三个 Source Generator，实现零反射代码生成
3. 确保所有生成的代码 100% IL2CPP 兼容

## 验收标准 (M4)

- [ ] `Atlas.Shared.dll` 在 .NET 10 和 Unity IL2CPP 上均可编译运行
- [ ] Entity Source Generator 生成序列化、脏标记、工厂代码
- [ ] RPC Source Generator 生成发送存根和接收分发代码
- [ ] Events Source Generator 生成事件注册代码
- [ ] 所有生成的代码中无 `System.Reflection`、`Activator.CreateInstance`、`MethodInfo.Invoke`

---

## 任务 4.1: `Atlas.Shared` 项目

### 新建目录: `src/csharp/Atlas.Shared/`

```
src/csharp/Atlas.Shared/
├── Atlas.Shared.csproj
├── Entity/
│   ├── IEntityDef.cs              # 实体定义标记接口
│   ├── Attributes.cs              # [Entity], [Replicated], [Persistent], [ServerOnly]
│   └── ISerializable.cs           # 序列化接口（见下方定义）
├── DataTypes/
│   ├── Vector3.cs                 # 纯 C# 数学（不依赖 NativeApi）
│   ├── Quaternion.cs
│   └── EntityRef.cs               # 实体引用（ID + 类型）
├── Serialization/
│   ├── SpanWriter.cs              # 二进制写入（与 C++ BinaryWriter 字节对齐）
│   └── SpanReader.cs              # 二进制读取（与 C++ BinaryReader 字节对齐）
├── Protocol/
│   ├── MessageIds.cs              # 消息 ID 常量
│   └── IRpcTarget.cs              # RPC 目标接口
├── Events/
│   ├── EventHandlerAttribute.cs   # [EventHandler("name")]
│   └── IEventListener.cs          # 事件监听器标记接口
└── Rpc/
    ├── ServerRpcAttribute.cs      # [ServerRpc]
    ├── ClientRpcAttribute.cs      # [ClientRpc]
    └── IRpcProxy.cs               # RPC 代理接口
```

### 项目文件: `Atlas.Shared.csproj`

```xml
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <!-- netstandard2.1: Unity 2021+ 兼容 -->
    <TargetFramework>netstandard2.1</TargetFramework>
    <LangVersion>latest</LangVersion>
    <Nullable>enable</Nullable>
    <RootNamespace>Atlas</RootNamespace>
  </PropertyGroup>

  <!-- Source Generators 作为 Analyzer 引用 -->
  <ItemGroup>
    <ProjectReference Include="../Atlas.Generators.Entity/Atlas.Generators.Entity.csproj"
                      OutputItemType="Analyzer"
                      ReferenceOutputAssembly="false" />
    <ProjectReference Include="../Atlas.Generators.Rpc/Atlas.Generators.Rpc.csproj"
                      OutputItemType="Analyzer"
                      ReferenceOutputAssembly="false" />
    <ProjectReference Include="../Atlas.Generators.Events/Atlas.Generators.Events.csproj"
                      OutputItemType="Analyzer"
                      ReferenceOutputAssembly="false" />
  </ItemGroup>
</Project>
```

### 关键约束

- [ ] Target `netstandard2.1` — Unity 2021+ 兼容
- [ ] **零外部依赖** — 不引用 `Atlas.Runtime`（它依赖 NativeApi/unsafe）
- [ ] `Atlas.Runtime` 引用 `Atlas.Shared`，反向引用禁止
- [ ] `Atlas.Shared` 中的 `Vector3`/`Quaternion` 是纯 C# 实现（不调用 Native）
- [ ] `Vector3`/`Quaternion` 必须实现 `IEquatable<T>`、`operator ==`/`!=`、`GetHashCode()`（脏标记 setter 中 `if (_position != value)` 依赖值相等比较）

---

### `ISerializable` 接口: `src/csharp/Atlas.Shared/Entity/ISerializable.cs`

> **⚠ ref struct 约束**: `SpanWriter`/`SpanReader` 是 `ref struct`，在 C# 标准中不能作为接口方法参数
> （因为接口可被装箱调用，而 ref struct 不能装箱）。C# 13 引入了 `allows ref struct` anti-constraint 解决此问题，
> 但 netstandard2.1 不支持。
>
> **推荐方案**: 不定义 `ISerializable` 接口。Source Generator 直接在 partial class 上生成
> `Serialize(ref SpanWriter)` 和 `Deserialize(ref SpanReader)` 方法（duck-typing）。
> 热重载和迁移代码通过 Generator 生成的已知方法名调用，无需接口约束。
> 如果需要编译期约束，可改用 abstract class 或将接口放在 `Atlas.Runtime`（net10.0，支持 C# 13）中。

```csharp
namespace Atlas.Entity;

/// <summary>
/// 实体序列化接口。由 Entity Source Generator 自动实现。
/// 用于状态持久化、热重载状态迁移、实体跨进程迁移等场景。
///
/// 注意: 如 Atlas.Shared 使用 netstandard2.1 且不支持 C# 13 allows ref struct，
/// 此接口需移至 Atlas.Runtime (net10.0) 或改用 duck-typing（见上方说明）。
/// </summary>
public interface ISerializable
{
    /// <summary>将实体全量状态序列化到 SpanWriter</summary>
    void Serialize(ref SpanWriter writer);

    /// <summary>从 SpanReader 反序列化恢复实体状态</summary>
    void Deserialize(ref SpanReader reader);
}
```

---

## 任务 4.2: 属性标记定义

### `src/csharp/Atlas.Shared/Entity/Attributes.cs`

```csharp
namespace Atlas.Entity;

[AttributeUsage(AttributeTargets.Class)]
public sealed class EntityAttribute : Attribute
{
    public string TypeName { get; }
    public EntityAttribute(string typeName) => TypeName = typeName;
}

[AttributeUsage(AttributeTargets.Field)]
public sealed class ReplicatedAttribute : Attribute
{
    /// <summary>
    /// 同步范围。默认 AllClients（所有客户端可见）。
    /// OwnClient 仅拥有者客户端可见（如 HP、背包）。
    /// </summary>
    public ReplicationScope Scope { get; set; } = ReplicationScope.AllClients;
}

public enum ReplicationScope : byte
{
    CellPrivate = 0, // 仅 Cell 端，不同步给任何客户端
    BaseOnly    = 1, // 仅 Base 端
    OwnClient   = 2, // Base/Cell + 拥有者客户端
    AllClients  = 3, // Base/Cell + 所有客户端
}

[AttributeUsage(AttributeTargets.Field)]
public sealed class PersistentAttribute : Attribute { }

[AttributeUsage(AttributeTargets.Field)]
public sealed class ServerOnlyAttribute : Attribute { }
```

### `src/csharp/Atlas.Shared/Rpc/ServerRpcAttribute.cs`

```csharp
namespace Atlas.Rpc;

[AttributeUsage(AttributeTargets.Method)]
public sealed class ServerRpcAttribute : Attribute
{
    public bool Reliable { get; set; } = true;
}

[AttributeUsage(AttributeTargets.Method)]
public sealed class ClientRpcAttribute : Attribute
{
    public bool Reliable { get; set; } = true;
}
```

### `src/csharp/Atlas.Shared/Events/EventHandlerAttribute.cs`

```csharp
namespace Atlas.Events;

[AttributeUsage(AttributeTargets.Method)]
public sealed class EventHandlerAttribute : Attribute
{
    public string EventName { get; }
    public EventHandlerAttribute(string eventName) => EventName = eventName;
}

public interface IEventListener { }
```

---

## 任务 4.3: `SpanWriter` / `SpanReader` 序列化基础设施

> 完整实现参见 [serialization_alignment.md](serialization_alignment.md)，此处仅展示 API 概览。

### `src/csharp/Atlas.Shared/Serialization/SpanWriter.cs` + `SpanReader.cs`

```csharp
namespace Atlas.Serialization;

public ref struct SpanWriter
{
    private byte[] _buffer;
    private int _position;

    public SpanWriter(int initialCapacity = 256) { ... }

    public void WriteBool(bool value) { ... }
    public void WriteInt32(int value) { ... }
    public void WriteInt64(long value) { ... }
    public void WriteFloat(float value) { ... }
    public void WriteDouble(double value) { ... }
    public void WriteString(string value) { ... }   // length-prefixed UTF-8
    public void WriteVector3(Vector3 value) { ... }
    public void WriteUInt32(uint value) { ... }

    public ReadOnlySpan<byte> WrittenSpan => _buffer.AsSpan(0, _position);
}

public ref struct SpanReader
{
    private readonly ReadOnlySpan<byte> _data;
    private int _position;

    public SpanReader(ReadOnlySpan<byte> data) { ... }

    public bool ReadBool() { ... }
    public int ReadInt32() { ... }
    public long ReadInt64() { ... }
    public float ReadFloat() { ... }
    public double ReadDouble() { ... }
    public string ReadString() { ... }
    public Vector3 ReadVector3() { ... }
    public uint ReadUInt32() { ... }
}
```

### 工作点

- [ ] 使用 `BinaryPrimitives` 做字节序处理（小端）
- [ ] `ref struct` 确保栈分配，零 GC 压力
- [ ] `WriteString`: packed_int(byte_len) + UTF-8 bytes（与 C++ `BinaryWriter::write_string` 格式完全一致，packed_int: `<0xFF` 为 1 字节，`>=0xFF` 为 `0xFF` + 4 字节 LE uint32）
- [ ] `ArrayPool<byte>` 缓冲区管理，`Dispose()` 归还
- [ ] 这是 Source Generator 生成的序列化代码的基础设施

---

## 任务 4.4: `Atlas.Generators.Entity` — 实体 Source Generator

### 新建目录: `src/csharp/Atlas.Generators.Entity/`

```
src/csharp/Atlas.Generators.Entity/
├── Atlas.Generators.Entity.csproj   # netstandard2.0
├── EntityGenerator.cs               # 主 Generator
├── Emitters/
│   ├── SerializationEmitter.cs      # 生成 Serialize/Deserialize
│   ├── DirtyTrackingEmitter.cs      # 生成脏标记
│   ├── FactoryEmitter.cs            # 生成工厂注册
│   └── DeltaSyncEmitter.cs          # 生成增量同步
└── Model/
    └── EntityModel.cs               # 解析后的实体模型
```

### 用户输入示例

```csharp
[Entity("Player")]
public partial class PlayerEntity : ServerEntity
{
    // 字段 + 生成属性模式:
    // 用户声明标记字段，Source Generator 生成对应的 public 属性。
    // 这种模式不需要 C# 13 partial property，兼容 Unity netstandard2.1。

    [Replicated(Scope = ReplicationScope.AllClients), Persistent]
    private string _name = "";

    [Replicated(Scope = ReplicationScope.OwnClient)]
    private float _health = 100f;

    [Replicated(Scope = ReplicationScope.AllClients)]
    private Vector3 _position;

    [Persistent]
    private int _totalPlayTime;

    [ServerOnly]
    private long _lastHeartbeat;
}
```

### Generator 输出 1: 序列化

```csharp
// <auto-generated by Atlas.Generators.Entity/>
public partial class PlayerEntity : ISerializable
{
    /// Source Generator 生成的类型名常量，override 基类 ServerEntity.TypeName
    public override string TypeName => "Player";

    private const int kSerializationVersion = 1;

    public void Serialize(ref SpanWriter writer)
    {
        writer.WriteInt32(kSerializationVersion);
        writer.WriteString(_name);
        writer.WriteFloat(_health);
        writer.WriteVector3(_position);
        writer.WriteInt32(_totalPlayTime);
        writer.WriteInt64(_lastHeartbeat);
    }

    public void Deserialize(ref SpanReader reader)
    {
        var version = reader.ReadInt32();
        _name = reader.ReadString();
        _health = reader.ReadFloat();
        _position = reader.ReadVector3();
        _totalPlayTime = reader.ReadInt32();
        if (version >= 1)
            _lastHeartbeat = reader.ReadInt64();
    }
}
```

**IL2CPP 安全**: 直接字段读写，无 `PropertyInfo.GetValue()`。

### Generator 输出 2: 脏标记 + 生成属性

```csharp
// <auto-generated/>
public partial class PlayerEntity
{
    [Flags]
    private enum ReplicatedDirtyFlags : uint
    {
        None     = 0,
        Name     = 1 << 0,
        Health   = 1 << 1,
        Position = 1 << 2,
    }

    private ReplicatedDirtyFlags _dirtyFlags;

    // Generator 为每个 [Replicated] 标记的字段生成 public 属性
    // 属性名 = 字段名去掉 _ 前缀并 PascalCase（_health → Health）
    public string Name
    {
        get => _name;
        set
        {
            if (_name != value)
            {
                _name = value;
                _dirtyFlags |= ReplicatedDirtyFlags.Name;
            }
        }
    }

    public float Health
    {
        get => _health;
        set
        {
            if (_health != value)
            {
                _health = value;
                _dirtyFlags |= ReplicatedDirtyFlags.Health;
            }
        }
    }

    public Vector3 Position
    {
        get => _position;
        set
        {
            if (_position != value)
            {
                _position = value;
                _dirtyFlags |= ReplicatedDirtyFlags.Position;
            }
        }
    }

    // [Persistent] only 字段也生成属性，但不带脏标记
    public int TotalPlayTime
    {
        get => _totalPlayTime;
        set => _totalPlayTime = value;
    }

    // [ServerOnly] 字段也生成属性，不带脏标记
    public long LastHeartbeat
    {
        get => _lastHeartbeat;
        set => _lastHeartbeat = value;
    }

    public bool IsDirty => _dirtyFlags != ReplicatedDirtyFlags.None;

    public void SerializeReplicatedDelta(ref SpanWriter writer)
    {
        writer.WriteUInt32((uint)_dirtyFlags);
        if ((_dirtyFlags & ReplicatedDirtyFlags.Name) != 0)
            writer.WriteString(_name);
        if ((_dirtyFlags & ReplicatedDirtyFlags.Health) != 0)
            writer.WriteFloat(_health);
        if ((_dirtyFlags & ReplicatedDirtyFlags.Position) != 0)
            writer.WriteVector3(_position);
    }

    public void ApplyReplicatedDelta(ref SpanReader reader)
    {
        var flags = (ReplicatedDirtyFlags)reader.ReadUInt32();
        if ((flags & ReplicatedDirtyFlags.Name) != 0)
            _name = reader.ReadString();
        if ((flags & ReplicatedDirtyFlags.Health) != 0)
            _health = reader.ReadFloat();
        if ((flags & ReplicatedDirtyFlags.Position) != 0)
            _position = reader.ReadVector3();
    }

    public void ClearDirty() => _dirtyFlags = ReplicatedDirtyFlags.None;
}
```

**设计要点**:
- 用户声明 `[Replicated] private float _health;`，Generator 生成 `public float Health` 属性
- 属性命名规则: 去掉 `_` 前缀，首字母大写（`_health` → `Health`）
- 仅 `[Replicated]` 属性生成脏标记 setter，`[Persistent]`/`[ServerOnly]` 生成普通 setter
- 不使用 `public new`（partial class 中不合法），直接从字段生成新属性

**IL2CPP 安全**: 编译期 `Flags` 枚举 + bit 操作，无运行时代理。

### Generator 输出 3: 工厂

```csharp
// <auto-generated/>
public static partial class EntityFactory
{
    private static readonly Dictionary<string, Func<ServerEntity>> _creators
        = new(StringComparer.Ordinal);

    static EntityFactory()
    {
        _creators["Player"] = static () => new PlayerEntity();
        // 其他 [Entity] 标记的类型也在此生成
    }

    public static ServerEntity? Create(string typeName)
    {
        return _creators.TryGetValue(typeName, out var creator) ? creator() : null;
    }
}
```

**IL2CPP 安全**: `static () => new T()` 编译期确定，无 `Activator.CreateInstance`。

### 工作点

- [ ] 实现 `IIncrementalGenerator`
- [ ] 扫描带 `[Entity]` 属性的 `partial class`
- [ ] 解析 `[Replicated]`, `[Persistent]`, `[ServerOnly]` 标记的 **private 字段**
- [ ] 为每个字段生成对应的 public 属性（命名: `_health` → `Health`）
- [ ] 为每个字段类型生成对应的 `SpanWriter.Write*` / `SpanReader.Read*` 调用
- [ ] 脏标记: 仅对 `[Replicated]` 字段的生成属性包含脏标记 setter
- [ ] 生成 `TypeName` 属性 override（`public override string TypeName => "Player";`）
- [ ] 工厂: 收集所有 `[Entity]` 类，生成注册代码
- [ ] 版本号: 基于属性列表的哈希或递增整数
- [ ] 编译期诊断:
  - `ATLAS_ENTITY001`: 类未标记 `partial`
  - `ATLAS_ENTITY002`: 不支持的字段类型
  - `ATLAS_ENTITY003`: `[Entity]` 类未继承 `ServerEntity`
  - `ATLAS_ENTITY004`: 标记字段非 `private`（建议使用 `private` 字段）
  - `ATLAS_ENTITY005`: 字段名不以 `_` 开头（无法推导属性名）

---

## 任务 4.5: `Atlas.Generators.Rpc` — RPC Source Generator

### 新建目录: `src/csharp/Atlas.Generators.Rpc/`

### 用户输入

```csharp
public partial class PlayerEntity : ServerEntity
{
    // 发送端: 用户声明 partial 方法，Generator 生成发送存根
    [ClientRpc(Reliable = true)]
    public partial void ShowDamageNumber(float amount, Vector3 position);

    [ServerRpc]
    public partial void RequestMove(Vector3 target);

    // 接收端: 用户实现 On 前缀的处理器方法
    // Generator 生成的 RpcDispatcher 反序列化参数后调用这些方法
    protected virtual void OnShowDamageNumber(float amount, Vector3 position)
    {
        // 客户端收到后在此处理（由用户实现）
    }

    protected virtual void OnRequestMove(Vector3 target)
    {
        // 服务端收到后在此处理（由用户实现）
    }
}
```

> **发送/接收模式**: 用户声明 `[ClientRpc] public partial void ShowDamageNumber(...)` 作为发送方法，
> Generator 生成序列化发送存根。接收端由用户实现 `OnShowDamageNumber(...)` 处理器方法，
> Generator 生成的 `RpcDispatcher` 反序列化参数后调用该方法。
>
> - 发送: `entity.ShowDamageNumber(100f, pos)` → Generator 存根序列化并发送
> - 接收: `RpcDispatcher` 反序列化 → 调用 `entity.OnShowDamageNumber(100f, pos)`

### Generator 输出: 发送存根

```csharp
// <auto-generated/>
public partial class PlayerEntity
{
    public partial void ShowDamageNumber(float amount, Vector3 position)
    {
        var writer = new SpanWriter(32);
        try
        {
            writer.WriteFloat(amount);
            writer.WriteVector3(position);
            // 使用 MailboxTarget.OwnerClient 作为默认目标
            // Mailbox 扩展（Phase 4.9）提供 AllClients/OtherClients 支持
            this.SendClientRpc(RpcIds.PlayerEntity_ShowDamageNumber,
                               MailboxTarget.OwnerClient, writer.WrittenSpan);
        }
        finally
        {
            writer.Dispose();
        }
    }
}
```

### Generator 输出: 接收分发

```csharp
// <auto-generated/>
internal static partial class RpcDispatcher
{
    public static void DispatchClientRpc(
        ServerEntity target, int rpcId, ref SpanReader reader)
    {
        switch (target)
        {
            case PlayerEntity player:
                DispatchPlayerEntity(player, rpcId, ref reader);
                break;
            // ... 其他实体类型
        }
    }

    private static void DispatchPlayerEntity(
        PlayerEntity target, int rpcId, ref SpanReader reader)
    {
        switch (rpcId)
        {
            case RpcIds.PlayerEntity_ShowDamageNumber:
            {
                var amount = reader.ReadFloat();
                var position = reader.ReadVector3();
                target.OnShowDamageNumber(amount, position);
                break;
            }
        }
    }
}
```

### Generator 输出: RPC ID

```csharp
// <auto-generated/>
// ID 格式: 0xTTTT_DDNN — TypeIndex(16bit) + Direction(8bit) + MethodIndex(8bit)
// Direction: ClientRpc=0x00, ServerRpc=0x01, CellRpc=0x02, BaseRpc=0x03
internal static class RpcIds
{
    // ClientRpc (Direction=0x00)
    public const int PlayerEntity_ShowDamageNumber = 0x0001_0001;

    // ServerRpc (Direction=0x01)
    public const int PlayerEntity_RequestMove      = 0x0001_0101;
}
```

### 工作点

- [ ] 扫描 `[ClientRpc]` / `[ServerRpc]` 标记的 `partial` 方法
- [ ] 为每个方法生成序列化存根（发送端）
- [ ] 为每个实体类型生成分发 switch（接收端）
- [ ] RPC ID 分配: 编译期顺序分配
  - 格式: `0xTTTT_DDNN` — TypeIndex(16bit) + Direction(8bit) + MethodIndex(8bit)
  - TypeIndex: 按 `[Entity]` 类的 `TypeName` 字典序排序后从 1 递增分配
  - Direction: ClientRpc=0x00, ServerRpc=0x01, CellRpc=0x02, BaseRpc=0x03
  - MethodIndex: 同一 (Entity, Direction) 下按方法名字典序从 1 递增分配
  - 添加/删除实体类型或方法可能改变已有 ID — 编译期诊断可检测客户端/服务端版本不匹配
- [ ] `Reliable` 属性控制传输方式
- [ ] 编译期诊断:
  - `ATLAS_RPC001`: 方法未标记 `partial`
  - `ATLAS_RPC002`: 不支持的参数类型
  - `ATLAS_RPC003`: 返回值必须为 `void`

---

## 任务 4.6: `Atlas.Generators.Events` — 事件 Source Generator

### 新建目录: `src/csharp/Atlas.Generators.Events/`

### 用户输入

```csharp
public partial class PlayerEntity : ServerEntity, IEventListener
{
    [EventHandler("player_level_up")]
    private void OnLevelUp(int newLevel, int oldLevel) { }

    [EventHandler("zone_changed")]
    private void OnZoneChanged(string zoneName) { }
}
```

### Generator 输出

```csharp
// <auto-generated/>
public partial class PlayerEntity
{
    // Source Generator 为每个 [EventHandler] 方法生成一个内部 IEventHandler 实现类。
    // 因为 SpanReader 是 ref struct，不能用于 Action<T> 或 lambda 参数，
    // 所以用 IEventHandler 接口接收 ReadOnlySpan<byte>，内部构造 SpanReader。

    private sealed class Handler_OnLevelUp : IEventHandler
    {
        private readonly PlayerEntity _owner;
        public Handler_OnLevelUp(PlayerEntity owner) => _owner = owner;

        public void Handle(ReadOnlySpan<byte> payload)
        {
            var r = new SpanReader(payload);
            var newLevel = r.ReadInt32();
            var oldLevel = r.ReadInt32();
            _owner.OnLevelUp(newLevel, oldLevel);
        }
    }

    private sealed class Handler_OnZoneChanged : IEventHandler
    {
        private readonly PlayerEntity _owner;
        public Handler_OnZoneChanged(PlayerEntity owner) => _owner = owner;

        public void Handle(ReadOnlySpan<byte> payload)
        {
            var r = new SpanReader(payload);
            var zoneName = r.ReadString();
            _owner.OnZoneChanged(zoneName);
        }
    }

    public void Atlas_RegisterEventHandlers(EventBus bus)
    {
        bus.Subscribe("player_level_up", new Handler_OnLevelUp(this), this);
        bus.Subscribe("zone_changed", new Handler_OnZoneChanged(this), this);
    }

    public void Atlas_UnregisterEventHandlers(EventBus bus)
    {
        bus.UnsubscribeAll(this);
    }
}
```

### EventBus 详细设计

EventBus 为**本地进程内同步分发**，不跨进程。跨进程事件通过 RPC/Mailbox 显式调用。

> **ref struct 约束**: `SpanReader` 是 `ref struct`，不能用作泛型参数（如 `Action<SpanReader>`）、
> 不能装箱、不能作为 lambda 参数。因此 EventBus 使用 `IEventHandler` 接口回调，
> Source Generator 为每个事件监听器生成实现类。

```csharp
namespace Atlas.Events;

/// <summary>
/// 事件处理器接口。SpanReader 是 ref struct，不能用于 Action&lt;T&gt; 泛型参数，
/// 因此使用接口方法接收 ReadOnlySpan&lt;byte&gt;，由实现方自行构造 SpanReader。
/// </summary>
public interface IEventHandler
{
    void Handle(ReadOnlySpan<byte> payload);
}

/// <summary>
/// 进程内同步事件总线。
/// 线程安全：否，仅在主线程（Tick 线程）调用。
/// 跨进程事件不通过 EventBus，使用 RPC/Mailbox 代替。
/// </summary>
public sealed class EventBus
{
    private readonly Dictionary<string, List<EventSubscription>> _subscriptions = new();

    /// <summary>订阅事件（由 Source Generator 生成的注册代码调用）</summary>
    public void Subscribe(string eventName, IEventHandler handler, object owner)
    {
        if (!_subscriptions.TryGetValue(eventName, out var list))
        {
            list = new List<EventSubscription>();
            _subscriptions[eventName] = list;
        }
        list.Add(new EventSubscription(handler, owner));
    }

    /// <summary>同步分发事件给所有订阅者</summary>
    public void Fire(string eventName, ReadOnlySpan<byte> payload)
    {
        if (!_subscriptions.TryGetValue(eventName, out var list))
            return;

        foreach (var sub in list)
        {
            try
            {
                sub.Handler.Handle(payload);
            }
            catch (Exception ex)
            {
                Atlas.Log.Error($"Event '{eventName}' handler failed: {ex.Message}");
            }
        }
    }

    /// <summary>移除某 owner 的所有事件订阅</summary>
    public void UnsubscribeAll(object owner)
    {
        foreach (var list in _subscriptions.Values)
        {
            list.RemoveAll(s => ReferenceEquals(s.Owner, owner));
        }
    }

    private readonly record struct EventSubscription(
        IEventHandler Handler, object Owner);
}
```

**设计要点**:
- 同步分发: `Fire()` 在调用线程上逐个执行 handler，保证事件处理顺序确定性
- Owner 追踪: 每个订阅关联 owner（通常是实体实例），实体销毁时可批量清除
- 异常隔离: 单个 handler 异常不影响其他订阅者
- 不跨进程: 进程间通信统一使用 RPC/Mailbox，避免 EventBus 承担分布式职责

### 工作点

- [ ] 扫描实现 `IEventListener` 且带 `[EventHandler]` 方法的类
- [ ] 生成 `Atlas_RegisterEventHandlers` 和 `Atlas_UnregisterEventHandlers`
- [ ] 参数反序列化: 根据方法签名生成 `SpanReader.Read*` 调用
- [ ] EventBus 实现: 本地进程内同步分发，Owner 追踪，异常隔离
- [ ] 实体销毁时自动调用 `EventBus.UnsubscribeAll(this)` 清除订阅
- [ ] 编译期诊断:
  - `ATLAS_EVENT001`: 类未实现 `IEventListener`
  - `ATLAS_EVENT002`: 不支持的参数类型

---

## 任务 4.7: Source Generator 测试

### 通用测试策略

每个 Generator 需要三类测试:

**1. 快照测试 (Snapshot Tests)**

```csharp
[Fact]
public async Task EntityGenerator_SimpleEntity_MatchesSnapshot()
{
    var source = @"
        [Entity(""Test"")]
        public partial class TestEntity : ServerEntity
        {
            [Replicated]
            private float _health = 100f;
        }";

    var output = await RunGenerator<EntityGenerator>(source);
    await Verify(output);  // 与 .verified.txt 文件比对
}
```

**2. 编译验证测试**

```csharp
[Fact]
public async Task GeneratedCode_CompilesWithoutErrors()
{
    var source = /* 完整的实体定义 */;
    var result = await CompileWithGenerator<EntityGenerator>(source);
    Assert.Empty(result.Diagnostics.Where(d => d.Severity == DiagnosticSeverity.Error));
}
```

**3. 诊断测试**

```csharp
[Fact]
public async Task NonPartialClass_EmitsDiagnostic()
{
    var source = @"
        [Entity(""Test"")]
        public class TestEntity : ServerEntity { }  // 缺少 partial
        ";
    var diagnostics = await GetDiagnostics<EntityGenerator>(source);
    Assert.Contains(diagnostics, d => d.Id == "ATLAS_ENTITY001");
}
```

### 测试矩阵

| Generator | 测试文件 | 关键用例 |
|-----------|---------|---------|
| Entity | `EntityGeneratorTests.cs` | 简单实体; 多属性; 嵌套类型; 继承链; 无属性标记; 不支持的类型 |
| Entity | `DirtyTrackingTests.cs` | 脏标记生成; 仅 [Replicated] 属性; ClearDirty |
| Entity | `FactoryTests.cs` | 多实体工厂注册; 重复 TypeName 诊断 |
| Rpc | `RpcGeneratorTests.cs` | ClientRpc/ServerRpc; 多参数; Reliable 标记; void 检查 |
| Rpc | `RpcIdTests.cs` | ID 唯一性; ID 稳定性（同一方法重新编译 ID 不变） |
| Events | `EventGeneratorTests.cs` | 事件注册/注销; 多事件; 参数类型 |

### IL2CPP 兼容性验证

```csharp
[Fact]
public async Task GeneratedCode_HasNoReflection()
{
    var source = /* 完整实体定义 */;
    var output = await RunGenerator<EntityGenerator>(source);

    // 确保生成的代码中无反射 API
    Assert.DoesNotContain("System.Reflection", output);
    Assert.DoesNotContain("Activator.CreateInstance", output);
    Assert.DoesNotContain("MethodInfo", output);
    Assert.DoesNotContain("PropertyInfo", output);
    Assert.DoesNotContain("GetType()", output);
    Assert.DoesNotContain("typeof(", output); // 允许 typeof 用于 dictionary key
}
```

---

## 任务 4.8: Unity 集成验证

### 工作点

- [ ] 创建最小 Unity 项目: `tests/unity/AtlasSharedTest/`
- [ ] 将 `Atlas.Shared.dll` 放入 `Assets/Plugins/`
- [ ] 编写使用实体定义的测试 MonoBehaviour
- [ ] 执行 IL2CPP 构建（iOS/Android target），验证零错误
- [ ] 文档记录集成步骤

### 验证清单

- [ ] `Atlas.Shared.dll` 在 Unity Editor 中无编译错误
- [ ] IL2CPP 构建无 stripping 问题（必要时添加 `link.xml`）
- [ ] 实体序列化/反序列化正确
- [ ] RPC 分发正确
- [ ] `Vector3` 与 Unity `Vector3` 可互转

---

## 任务依赖图

```
4.1 Atlas.Shared 项目 ──── 4.2 属性标记定义
        │                        │
        │                 4.3 SpanIO 基础设施
        │                        │
        ├── 4.4 Entity Generator ─┤
        │                        │
        ├── 4.5 RPC Generator ───┤
        │                        │
        ├── 4.6 Events Generator ─┤
        │                        │
        └── 4.7 Generator 测试 ──┘
                │
                ▼
         4.8 Unity 集成验证
```

**建议执行顺序**: 4.1 + 4.2 → 4.3 → 4.4 (最重要) → 4.5 → 4.6 → 4.7 → 4.8

---

## 新增任务: Mailbox 代理 + 实体类型注册

> 以下任务源自架构讨论中产生的三个核心设计问题，已独立成文。

### 任务 4.9: Mailbox 代理机制

**详细设计文档**: [entity_mailbox_design.md](entity_mailbox_design.md)

为每种 RPC 方向生成 `readonly struct` 代理，复现 BigWorld `entity.client.SayHi()` 风格调用：

- [ ] RPC Attribute 扩展: `CellRpcAttribute`, `BaseRpcAttribute`, `MailboxTarget` 枚举
- [ ] RPC Generator 扩展 — 生成 `{Entity}ClientMailbox`, `{Entity}CellMailbox`, `{Entity}BaseMailbox` 代理 struct
- [ ] RPC Generator 扩展 — 生成实体属性 `Client`, `AllClients`, `OtherClients`, `Cell`, `Base`
- [ ] RPC Generator 扩展 — 生成接收端 `RpcDispatcher` 分发 switch
- [ ] ServerEntity 基类 `SendClientRpc` / `SendCellRpc` / `SendBaseRpc` 方法
- [ ] C++ `clr_rpc_router` — RPC 路由、校验、转发
- [ ] RPC ID 分配策略及冲突检测
- [ ] 单元测试: 零分配验证、序列化往返、分发覆盖

### 任务 4.10: 实体类型注册（替代 .def）

**详细设计文档**: [entity_type_registration.md](entity_type_registration.md)

C# Source Generator 编译期收集实体类型信息，启动时注册到 C++ `EntityDefRegistry`：

- [ ] C++ `EntityTypeDescriptor` / `PropertyDescriptor` / `RpcDescriptor` 数据结构
- [ ] C++ `EntityDefRegistry` — 注册、查找、RPC 校验、属性过滤
- [ ] C++ 导出函数实现 — `atlas_register_entity_type` / `atlas_unregister_all_entity_types`
- [ ] Entity Generator 扩展 — 生成 `EntityTypeRegistry.RegisterAll()` 注册代码
- [ ] 引擎各模块适配: RPC 路由、属性同步、DBApp 持久化、空间管理
- [ ] 热重载集成: 卸载前清除 → 重载后重新注册
- [ ] C++ 单元测试 + C# Generator 快照测试
