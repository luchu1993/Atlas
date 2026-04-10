# Phase 12: 客户端 SDK

> 前置依赖: Phase 9 (LoginApp), Phase 10 (CellApp AOI), Script Phase 4 (Atlas.Shared)
> BigWorld 参考: `lib/connection/server_connection.hpp`, `lib/connection/client_interface.hpp`, `lib/connection/avatar_filter_helper.hpp`

---

## 目标

提供纯 C# 客户端 SDK（`Atlas.ClientSDK`），使 Unity / Godot / 自定义引擎可以连接 Atlas 集群，
完成登录、实体同步、RPC 通信和位置插值。

**核心优势:** 客户端与服务端共享 `Atlas.Shared` 库（netstandard2.1），
实体定义、序列化代码、RPC 代理在编译期由 Source Generator 统一生成，零反射、IL2CPP 安全。

## 验收标准

- [ ] `Atlas.ClientSDK` 可发布为 NuGet 包，Unity 可引用
- [ ] 客户端可登录 Atlas 集群（LoginApp → BaseApp 完整流程）
- [ ] 客户端可接收 AOI 内实体的创建/销毁/属性更新
- [ ] 客户端可调用 `[ServerRpc]` 方法（编译期类型安全）
- [ ] 客户端可接收 `[ClientRpc]` 调用（Source Generator 分发）
- [ ] `AvatarFilter` 位置插值工作平滑（延迟自适应）
- [ ] 断线检测可工作，提供重连 API
- [ ] 带宽统计可查询
- [ ] 独立的命令行测试客户端（`atlas_test_client`）可验证全部功能
- [ ] 全部新增代码有单元测试

---

## 1. BigWorld 架构分析与 Atlas 适配

### 1.1 BigWorld 客户端 SDK 核心机制

| 机制 | BigWorld 实现 | 说明 |
|------|-------------|------|
| **语言** | C++ `ServerConnection` 类 | 客户端也是 C++ |
| **传输** | UDP (主) / TCP / WebSocket | 可切换 |
| **登录** | `LoginHandler` 状态机 + RSA 加密 | 与 LoginApp 交互 |
| **实体选择** | `selectEntity` / `selectAliasedEntity` 隐式状态 | 后续消息作用于选中实体 |
| **ID 别名** | `IDAlias` (uint8, 256 个) 节省带宽 | 频繁引用实体用 1 字节 |
| **位置压缩** | 24 种 `avatarUpdate` 变体 | Packed 位置/方向组合 |
| **位置插值** | `AvatarFilterHelper` 环形缓冲 + 延迟自适应 | 8 个采样点 |
| **属性同步** | `entityProperty` / `updateEntity` 消息 | 按属性 ID 更新 |
| **方法调用** | `entityMethod` 消息 (exposedMethodID) | 映射到脚本方法 |
| **AOI 进入** | `enterAoI` → `requestEntityUpdate` → `createEntity` | 三步创建 |
| **缓存戳** | `CacheStamps` (EventNumber per property) | 重入 AOI 时只发增量 |
| **带宽** | `bandwidthNotification` 服务器下行 | `downloadRate` 客户端控制 |

### 1.2 Atlas 适配决策

| 方面 | Atlas 决策 | 原因 |
|------|-----------|------|
| **语言** | 纯 C#（`Atlas.ClientSDK`）| Unity/IL2CPP 原生，共享 Atlas.Shared |
| **传输** | TCP（初期）| 与服务端一致，简化实现；后续可加 KCP |
| **登录** | `LoginClient` async/await | C# 异步模式 |
| **实体选择** | 每条消息携带 EntityID | TCP 不需要带宽极致优化 |
| **ID 别名** | 初期不实现 | TCP 开销可接受；后续优化 |
| **位置压缩** | 初期全精度 (3×float) | TCP 带宽充裕；后续可加压缩 |
| **位置插值** | `AvatarFilter` 对齐 BigWorld 算法 | 核心体验 |
| **属性同步** | `ApplyReplicatedDelta()` (Source Generator) | 与服务端共享序列化代码 |
| **方法调用** | `RpcDispatcher.DispatchClientRpc()` (Source Generator) | 编译期分发 |
| **AOI 进入** | `EntityEnter` 一步创建（含完整属性） | 简化协议 |
| **缓存戳** | 初期不实现 | 后续优化 |
| **带宽** | 服务端 Witness 带宽控制 | 客户端无需主动限速 |

### 1.3 Atlas.Shared 共享架构

```
┌────────────────────────────────────────────────────────────────────┐
│                    Atlas.Shared (netstandard2.1)                    │
│                                                                    │
│  [Entity] classes  │  SpanWriter/Reader  │  Attributes  │  Vector3 │
│                    │                     │              │          │
│  Source Generator 在两端分别生成:                                    │
│  ┌──────────────────────┐  ┌──────────────────────────┐           │
│  │ 服务端生成            │  │ 客户端生成                │           │
│  │ SerializeReplicatedΔ │  │ ApplyReplicatedDelta     │           │
│  │ [ClientRpc] 发送存根  │  │ [ClientRpc] 接收分发     │           │
│  │ [ServerRpc] 接收分发  │  │ [ServerRpc] 发送存根     │           │
│  │ EntityFactory         │  │ EntityFactory            │           │
│  └──────────────────────┘  └──────────────────────────┘           │
└────────────────────────────────────────────────────────────────────┘
        ↓                              ↓
  Atlas.Runtime (net9.0)        Atlas.ClientSDK (netstandard2.1)
  服务端 JIT                     Unity IL2CPP / .NET
```

**编译条件区分:**
```xml
<!-- Atlas.ClientSDK.csproj -->
<DefineConstants>ATLAS_CLIENT</DefineConstants>

<!-- Atlas.Runtime.csproj (服务端) -->
<DefineConstants>ATLAS_SERVER</DefineConstants>
```

Source Generator 检查 `ATLAS_CLIENT` / `ATLAS_SERVER` 条件生成不同方向的代码。

---

## 2. 协议设计

### 2.1 客户端 → 服务器消息

| 消息 | ID | 目标 | 用途 |
|------|-----|------|------|
| `LoginRequest` | 5000 | LoginApp | 登录（已在 Phase 9 定义） |
| `Authenticate` | 10000 | BaseApp | SessionKey 认证（已在 Phase 8 定义） |
| `ServerRpcCall` | 10001 | BaseApp | 调用 `[ServerRpc]`（已定义） |
| `EnableEntities` | 10002 | BaseApp | 开启 AOI 同步（已定义） |
| `AvatarUpdate` | 10010 | BaseApp → CellApp | 客户端位置更新 |
| `Heartbeat` | 10003 | BaseApp | 心跳（已定义） |
| `Disconnect` | 10020 | BaseApp | 主动断开 |

### 2.2 服务器 → 客户端消息

| 消息 | ID | 来源 | 用途 |
|------|-----|------|------|
| `LoginResult` | 5001 | LoginApp | 登录结果（已定义） |
| `AuthResult` | 10100 | BaseApp | 认证结果（已定义） |
| `CreateBasePlayer` | 10105 | BaseApp | 创建玩家 Base 实体 |
| `CreateCellPlayer` | 10110 | BaseApp | 创建玩家 Cell 实体（位置+属性） |
| `EntityEnter` | 10102 | BaseApp | AOI 实体进入（类型+位置+属性） |
| `EntityLeave` | 10103 | BaseApp | AOI 实体离开 |
| `EntityPositionUpdate` | 10111 | BaseApp | 实体位置/方向更新 |
| `EntityPropertyUpdate` | 10104 | BaseApp | `[Replicated]` 属性增量 |
| `ClientRpcCall` | 10101 | BaseApp | `[ClientRpc]` 方法调用 |
| `ResetEntities` | 10106 | BaseApp | giveClientTo 后重置 |
| `ForcedPosition` | 10112 | BaseApp | 服务器位置校正 |
| `LoggedOff` | 10113 | BaseApp | 被踢下线 |

### 2.3 详细消息定义

```csharp
// Atlas.ClientSDK / Protocol / Messages.cs
namespace Atlas.Client.Protocol;

// ========== 服务器 → 客户端 ==========

public struct CreateBasePlayer {
    public uint EntityId;
    public ushort TypeId;
    public byte[] Properties;      // [BASE_AND_CLIENT] 属性 blob
}

public struct CreateCellPlayer {
    public uint SpaceId;
    public Vector3 Position;
    public Vector3 Direction;
    public byte[] CellProperties;  // [OWN_CLIENT] + [ALL_CLIENTS] 属性 blob
}

public struct EntityEnter {
    public uint EntityId;
    public ushort TypeId;
    public Vector3 Position;
    public Vector3 Direction;
    public byte[] Properties;      // [ALL_CLIENTS] 属性 blob
}

public struct EntityLeave {
    public uint EntityId;
}

public struct EntityPositionUpdate {
    public uint EntityId;
    public Vector3 Position;
    public Vector3 Direction;
    public bool OnGround;
}

public struct EntityPropertyUpdate {
    public uint EntityId;
    public byte Scope;             // ReplicationScope
    public byte[] Delta;           // SerializeReplicatedDelta() 输出
}

public struct ClientRpcCall {
    public uint EntityId;          // 目标实体（可能是自己或 AOI 内其他实体）
    public uint RpcId;
    public byte[] Payload;
}

public struct ForcedPosition {
    public uint EntityId;
    public Vector3 Position;
    public Vector3 Direction;
}

// ========== 客户端 → 服务器 ==========

public struct AvatarUpdate {
    public Vector3 Position;
    public Vector3 Direction;
    public bool OnGround;
}

public struct Disconnect {
    public byte Reason;            // 0=normal, 1=timeout
}
```

---

## 3. 核心模块设计

### 3.1 AtlasClient — 主入口

```csharp
// Atlas.ClientSDK / AtlasClient.cs
namespace Atlas.Client;

/// <summary>
/// Atlas 客户端 SDK 主入口。
/// 线程安全: 所有方法必须在同一线程调用（Unity 主线程）。
/// </summary>
public sealed class AtlasClient : IDisposable
{
    // ========== 生命周期 ==========

    /// <summary>连接并登录</summary>
    public async Task<LoginResult> ConnectAsync(string loginHost, int loginPort,
                                                  string username, string password,
                                                  CancellationToken ct = default);

    /// <summary>断开连接</summary>
    public void Disconnect();

    /// <summary>每帧调用（处理收到的消息、更新插值）</summary>
    public void Update(float deltaTime);

    // ========== 状态 ==========

    public bool IsConnected { get; }
    public uint PlayerId { get; }             // 玩家实体 ID

    // ========== 实体管理 ==========

    public ClientEntityManager Entities { get; }

    // ========== RPC ==========

    /// <summary>调用 [ServerRpc] (由 Source Generator 的 Mailbox 代理调用)</summary>
    public void SendServerRpc(uint entityId, uint rpcId,
                               ReadOnlySpan<byte> payload);

    // ========== 位置 ==========

    /// <summary>发送本地玩家位置到服务器</summary>
    public void SendAvatarUpdate(Vector3 position, Vector3 direction,
                                   bool onGround);

    // ========== 事件 ==========

    public event Action? Connected;
    public event Action<string>? Disconnected;       // reason
    public event Action<uint>? ForcedPositionReceived; // entityId

    // ========== 带宽统计 ==========

    public long BytesReceived { get; }
    public long BytesSent { get; }
}
```

### 3.2 ClientEntityManager — 客户端实体管理

```csharp
// Atlas.ClientSDK / ClientEntityManager.cs
namespace Atlas.Client;

public sealed class ClientEntityManager
{
    // ========== 实体访问 ==========

    public ClientEntity? GetEntity(uint entityId);
    public ClientEntity? PlayerEntity { get; }
    public int EntityCount { get; }

    public IEnumerable<ClientEntity> AllEntities { get; }

    // ========== 事件 ==========

    /// <summary>玩家 Base 实体创建</summary>
    public event Action<ClientEntity>? PlayerBaseCreated;

    /// <summary>玩家 Cell 实体创建（进入世界）</summary>
    public event Action<ClientEntity>? PlayerCellCreated;

    /// <summary>AOI 实体进入</summary>
    public event Action<ClientEntity>? EntityEntered;

    /// <summary>AOI 实体离开</summary>
    public event Action<uint>? EntityLeft;

    /// <summary>实体属性更新</summary>
    public event Action<ClientEntity>? EntityUpdated;

    /// <summary>实体位置更新</summary>
    public event Action<ClientEntity>? EntityMoved;

    /// <summary>实体重置（giveClientTo）</summary>
    public event Action? EntitiesReset;

    // ========== 内部消息处理 ==========

    internal void HandleCreateBasePlayer(CreateBasePlayer msg);
    internal void HandleCreateCellPlayer(CreateCellPlayer msg);
    internal void HandleEntityEnter(EntityEnter msg);
    internal void HandleEntityLeave(EntityLeave msg);
    internal void HandleEntityPositionUpdate(EntityPositionUpdate msg);
    internal void HandleEntityPropertyUpdate(EntityPropertyUpdate msg);
    internal void HandleClientRpc(ClientRpcCall msg);
    internal void HandleResetEntities();
    internal void HandleForcedPosition(ForcedPosition msg);

    // ========== 内部 ==========

    private readonly Dictionary<uint, ClientEntity> entities_ = new();
    private ClientEntity? playerEntity_;
}
```

### 3.3 ClientEntity — 客户端实体

```csharp
// Atlas.ClientSDK / ClientEntity.cs
namespace Atlas.Client;

/// <summary>
/// 客户端侧的实体表示。
/// 包含服务端同步的属性 + 位置插值。
/// 底层实体类由 Atlas.Shared 的 [Entity] 类定义。
/// </summary>
public sealed class ClientEntity
{
    public uint EntityId { get; }
    public ushort TypeId { get; }
    public string TypeName { get; }

    /// <summary>Atlas.Shared 中定义的实体实例 (由 EntityFactory 创建)</summary>
    public ServerEntity SharedEntity { get; }

    // ========== 位置 (原始服务器值) ==========

    public Vector3 ServerPosition { get; internal set; }
    public Vector3 ServerDirection { get; internal set; }
    public bool OnGround { get; internal set; }

    // ========== 位置 (插值后，用于渲染) ==========

    public Vector3 FilteredPosition => filter_.GetFilteredPosition(currentTime_);
    public Vector3 FilteredDirection => filter_.GetFilteredDirection(currentTime_);

    // ========== 插值器 ==========

    public AvatarFilter Filter => filter_;

    // ========== 是否为本地玩家 ==========

    public bool IsPlayer { get; internal set; }

    internal AvatarFilter filter_ = new();
    internal float currentTime_;

    internal ClientEntity(uint entityId, ushort typeId, string typeName,
                           ServerEntity sharedEntity)
    {
        EntityId = entityId;
        TypeId = typeId;
        TypeName = typeName;
        SharedEntity = sharedEntity;
    }
}
```

### 3.4 AvatarFilter — 位置插值（对齐 BigWorld AvatarFilterHelper）

```csharp
// Atlas.ClientSDK / AvatarFilter.cs
namespace Atlas.Client;

/// <summary>
/// 位置插值器。对齐 BigWorld AvatarFilterHelper 算法:
/// - 环形缓冲区存储最近 N 个服务器采样点
/// - 延迟自适应（动态调整回放延迟以平滑抖动）
/// - 线性插值 + 外推限制
/// </summary>
public sealed class AvatarFilter
{
    private const int BufferSize = 8;

    // ========== 输入 ==========

    /// <summary>收到服务器位置更新时调用</summary>
    public void Input(float serverTime, Vector3 position, Vector3 direction,
                       bool onGround)
    {
        var sample = new Sample(serverTime, position, direction, onGround);
        buffer_[writeIndex_ % BufferSize] = sample;
        writeIndex_++;
        sampleCount_ = Math.Min(sampleCount_ + 1, BufferSize);

        UpdateIdealLatency();
    }

    // ========== 输出 ==========

    /// <summary>每帧调用，返回插值后的位置</summary>
    public Vector3 GetFilteredPosition(float clientTime)
    {
        float playbackTime = clientTime - latency_;
        return InterpolatePosition(playbackTime);
    }

    public Vector3 GetFilteredDirection(float clientTime)
    {
        float playbackTime = clientTime - latency_;
        return InterpolateDirection(playbackTime);
    }

    // ========== 延迟自适应 (对齐 BigWorld) ==========

    private void UpdateIdealLatency()
    {
        if (sampleCount_ < 2) return;

        // 计算采样间隔
        var prev = buffer_[(writeIndex_ - 2) % BufferSize];
        var curr = buffer_[(writeIndex_ - 1) % BufferSize];
        float interval = curr.Time - prev.Time;

        // 理想延迟 = latencyFrames × 采样间隔
        idealLatency_ = Math.Max(LatencyFrames * interval, MinLatency);
    }

    /// <summary>每帧调整实际延迟向理想值收敛</summary>
    public void UpdateLatency(float dt)
    {
        float diff = idealLatency_ - latency_;
        float speed = LatencyVelocity * MathF.Pow(MathF.Abs(diff), LatencyCurvePower);
        float maxMove = speed * dt;

        if (MathF.Abs(diff) <= maxMove)
            latency_ = idealLatency_;
        else
            latency_ += MathF.Sign(diff) * maxMove;
    }

    // ========== 配置 (对齐 BigWorld 默认值) ==========

    public float LatencyFrames { get; set; } = 2.0f;
    public float MinLatency { get; set; } = 0.1f;           // 100ms
    public float LatencyVelocity { get; set; } = 1.0f;      // s/s
    public float LatencyCurvePower { get; set; } = 2.0f;

    // ========== 内部 ==========

    private readonly Sample[] buffer_ = new Sample[BufferSize];
    private int writeIndex_ = 0;
    private int sampleCount_ = 0;
    private float latency_ = 0.2f;        // 初始延迟 200ms
    private float idealLatency_ = 0.2f;

    private Vector3 InterpolatePosition(float time) { /* 线性插值两个包围 time 的采样 */ }
    private Vector3 InterpolateDirection(float time) { /* 同上 */ }

    private record struct Sample(float Time, Vector3 Position,
                                  Vector3 Direction, bool OnGround);
}
```

### 3.5 NetworkTransport — 网络传输层

```csharp
// Atlas.ClientSDK / Network / NetworkTransport.cs
namespace Atlas.Client.Network;

/// <summary>
/// TCP 网络传输。
/// 帧格式: [uint32 frame_length LE][message data] (与服务端 TcpChannel 一致)
/// </summary>
internal sealed class TcpTransport : IDisposable
{
    public async Task ConnectAsync(string host, int port, CancellationToken ct);
    public void Disconnect();
    public bool IsConnected { get; }

    /// <summary>发送消息</summary>
    public void Send(ReadOnlySpan<byte> data);

    /// <summary>非阻塞接收 (在 Update 中调用)</summary>
    public bool TryReceive(out ReadOnlyMemory<byte> frame);

    /// <summary>统计</summary>
    public long BytesSent { get; }
    public long BytesReceived { get; }

    // 内部: Socket + 收发缓冲区
}
```

### 3.6 LoginClient — 登录流程

```csharp
// Atlas.ClientSDK / LoginClient.cs
namespace Atlas.Client;

internal sealed class LoginClient
{
    /// <summary>
    /// 完整登录流程:
    /// 1. TCP 连接 LoginApp
    /// 2. 发送 LoginRequest
    /// 3. 等待 LoginResult
    /// 4. 如果成功: TCP 连接 BaseApp
    /// 5. 发送 Authenticate(sessionKey)
    /// 6. 等待 AuthResult
    /// 7. 返回已认证的 BaseApp 连接
    /// </summary>
    public async Task<LoginResult> LoginAsync(
        string loginHost, int loginPort,
        string username, string password,
        CancellationToken ct)
    {
        // Step 1-3: LoginApp
        using var loginTransport = new TcpTransport();
        await loginTransport.ConnectAsync(loginHost, loginPort, ct);

        var loginReq = new LoginRequest { Username = username,
            PasswordHash = ComputeHash(password) };
        SendMessage(loginTransport, loginReq);

        var loginResult = await ReceiveMessage<LoginResult>(loginTransport, ct);
        if (loginResult.Status != LoginStatus.Success)
            return new LoginResult(false, null, loginResult.ErrorMessage);

        loginTransport.Disconnect();

        // Step 4-6: BaseApp
        var baseTransport = new TcpTransport();
        await baseTransport.ConnectAsync(
            loginResult.BaseAppAddr.Host, loginResult.BaseAppAddr.Port, ct);

        var authReq = new Authenticate { SessionKey = loginResult.SessionKey };
        SendMessage(baseTransport, authReq);

        var authResult = await ReceiveMessage<AuthResult>(baseTransport, ct);
        if (!authResult.Success)
        {
            baseTransport.Disconnect();
            return new LoginResult(false, null, "Authentication failed");
        }

        return new LoginResult(true, baseTransport, null);
    }
}
```

---

## 4. 端到端流程

### 4.1 登录 + 进入世界

```
Unity Client                   LoginApp     BaseApp      CellApp
  │                               │            │            │
  │ AtlasClient.ConnectAsync()    │            │            │
  │── LoginRequest ──────────────→│            │            │
  │←── LoginResult(ok, key, addr)─│            │            │
  │                               │            │            │
  │── TCP connect ────────────────────────────→│            │
  │── Authenticate(key) ──────────────────────→│            │
  │←── AuthResult(ok, entityId) ──────────────│            │
  │                                            │            │
  │←── CreateBasePlayer ──────────────────────│            │
  │    (entityId, typeId, base props)          │            │
  │    → PlayerBaseCreated event               │            │
  │                                            │            │
  │── EnableEntities ─────────────────────────→│            │
  │                                            │── create ─→│
  │←── CreateCellPlayer ──────────────────────│←── ack ───│
  │    (spaceId, pos, dir, cell props)         │            │
  │    → PlayerCellCreated event               │            │
  │                                            │            │
  │←── EntityEnter (nearby entities) ─────────│←── AOI ──│
  │    → EntityEntered event (per entity)      │            │
```

### 4.2 游戏中同步

```
Unity Client                   BaseApp          CellApp
  │                               │                │
  │ 玩家移动:                      │                │
  │── AvatarUpdate(pos, dir) ────→│── forward ────→│
  │                               │                │ entity.set_position()
  │                               │                │ RangeList shuffle
  │                               │                │ Witness::update()
  │                               │                │
  │←── EntityPositionUpdate ─────│←───────────────│ (其他实体位置)
  │    → AvatarFilter.Input()     │                │
  │    → EntityMoved event        │                │
  │                               │                │
  │ C# 属性变更 (服务端):          │                │
  │←── EntityPropertyUpdate ─────│←───────────────│ (DirtyFlags delta)
  │    → entity.ApplyReplicatedDelta()             │
  │    → EntityUpdated event      │                │
  │                               │                │
  │ 调用 [ServerRpc]:             │                │
  │── ServerRpcCall(rpcId, args) →│── validate ───→│
  │                               │                │ C# RpcDispatcher
  │                               │                │ → entity.OnMethod()
  │                               │                │
  │ 收到 [ClientRpc]:             │                │
  │←── ClientRpcCall(rpcId, args)─│←───────────────│
  │    → RpcDispatcher.DispatchClientRpc()         │
  │    → entity.OnShowDamage()    │                │
```

---

## 5. Source Generator 客户端集成

### 5.1 条件编译

Source Generator 根据 `ATLAS_CLIENT` / `ATLAS_SERVER` 生成不同代码：

```csharp
// [ServerRpc] — 服务端: 接收分发; 客户端: 发送存根
[Entity("Avatar")]
public partial class Avatar : ServerEntity
{
    [ServerRpc]
    public partial void RequestMove(Vector3 target);
}

// 服务端生成 (ATLAS_SERVER):
public partial void RequestMove(Vector3 target)
{
    // 反序列化参数 → 调用 OnRequestMove()
    // (实际由 RpcDispatcher 分发)
}

// 客户端生成 (ATLAS_CLIENT):
public partial void RequestMove(Vector3 target)
{
    var writer = new SpanWriter(32);
    writer.WriteVector3(target);
    AtlasClient.Current.SendServerRpc(EntityId,
        RpcIds.Avatar_RequestMove, writer.WrittenSpan);
    writer.Dispose();
}
```

### 5.2 客户端 RPC 接收

```csharp
// [ClientRpc] — 服务端: 发送存根; 客户端: 接收分发

// 客户端生成 (ATLAS_CLIENT):
internal static partial class ClientRpcDispatcher
{
    public static void Dispatch(ServerEntity target, uint rpcId,
                                 ref SpanReader reader)
    {
        switch (target)
        {
            case Avatar avatar:
                DispatchAvatar(avatar, rpcId, ref reader);
                break;
        }
    }

    private static void DispatchAvatar(Avatar target, uint rpcId,
                                        ref SpanReader reader)
    {
        switch (rpcId)
        {
            case RpcIds.Avatar_ShowDamage:
                var amount = reader.ReadFloat();
                var hitPos = reader.ReadVector3();
                target.OnShowDamage(amount, hitPos);
                break;
        }
    }
}
```

### 5.3 ApplyReplicatedDelta

```csharp
// 客户端生成 (ATLAS_CLIENT):
public partial class Avatar
{
    /// <summary>应用服务器发来的属性增量</summary>
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
}
```

---

## 6. 实现步骤

### Step 12.1: 网络传输层

**新增文件:**
```
src/csharp/Atlas.ClientSDK/
├── Atlas.ClientSDK.csproj
├── Network/
│   └── TcpTransport.cs
tests/csharp/Atlas.ClientSDK.Tests/
├── TcpTransportTests.cs
```

**实现:** TCP 连接/断开、帧编解码（`[uint32 len][data]`）、收发缓冲区、统计。

### Step 12.2: 消息编解码

**新增文件:**
```
src/csharp/Atlas.ClientSDK/
├── Protocol/
│   ├── MessageCodec.cs            # 消息 ID + SpanReader/Writer 编解码
│   ├── ClientMessages.cs          # 客户端→服务器消息定义
│   └── ServerMessages.cs          # 服务器→客户端消息定义
```

复用 `Atlas.Shared` 的 `SpanWriter` / `SpanReader`。消息格式与 C++ 侧 `NetworkMessage` 字节级兼容。

### Step 12.3: LoginClient

**新增文件:**
```
src/csharp/Atlas.ClientSDK/
├── LoginClient.cs
tests/csharp/Atlas.ClientSDK.Tests/
├── LoginClientTests.cs
```

### Step 12.4: ClientEntityManager + ClientEntity

**新增文件:**
```
src/csharp/Atlas.ClientSDK/
├── ClientEntityManager.cs
├── ClientEntity.cs
tests/csharp/Atlas.ClientSDK.Tests/
├── ClientEntityManagerTests.cs
```

### Step 12.5: AvatarFilter

**新增文件:**
```
src/csharp/Atlas.ClientSDK/
├── AvatarFilter.cs
tests/csharp/Atlas.ClientSDK.Tests/
├── AvatarFilterTests.cs
```

**测试用例（关键）:**
- 2 个采样点 → 线性插值正确
- 延迟自适应: 间隔变化 → idealLatency 调整
- 外推限制: 超出最新采样不过度外推
- 缓冲区环形写满 → 旧采样被覆盖

### Step 12.6: AtlasClient 主入口

**新增文件:**
```
src/csharp/Atlas.ClientSDK/
├── AtlasClient.cs
```

集成 LoginClient + Transport + EntityManager + 消息分发循环。

### Step 12.7: Source Generator 客户端条件

**更新文件:**
```
src/csharp/Atlas.Generators.Entity/     (更新: 检查 ATLAS_CLIENT)
src/csharp/Atlas.Generators.Rpc/        (更新: 检查 ATLAS_CLIENT)
```

- `ATLAS_CLIENT`: 生成 `ApplyReplicatedDelta()`、`[ServerRpc]` 发送存根、`[ClientRpc]` 接收分发
- `ATLAS_SERVER`: 生成 `SerializeReplicatedDelta()`、`[ClientRpc]` 发送存根、`[ServerRpc]` 接收分发

### Step 12.8: atlas_test_client 命令行工具

**新增文件:**
```
src/tools/atlas_test_client/
├── Program.cs
├── atlas_test_client.csproj
```

命令行客户端，用于集成测试和压力测试：
```bash
atlas_test_client --host 127.0.0.1 --port 20018 --user test1 --pass 123
# 自动: 登录 → 创建角色 → 移动 → 打印 AOI 实体 → 断开
```

### Step 12.9: 集成测试

**新增文件:**
```
tests/integration/test_client_sdk.cpp    # C++ 集成测试 (启动服务器集群)
tests/csharp/Atlas.ClientSDK.Tests/
├── IntegrationTests.cs                   # C# 端到端测试
```

端到端场景：
1. 启动完整集群 (machined + DBApp + BaseAppMgr + BaseApp + CellAppMgr + CellApp + LoginApp)
2. C# 客户端登录
3. 收到 CreateBasePlayer → CreateCellPlayer
4. 发送 EnableEntities → 收到 AOI 实体
5. 发送 AvatarUpdate → 收到其他实体的 PositionUpdate
6. 发送 ServerRpc → 服务端 C# 处理
7. 服务端 ClientRpc → 客户端 C# 收到
8. 属性变更 → 客户端 ApplyReplicatedDelta
9. 断线 → Disconnected 事件
10. 重连流程

---

## 7. 文件清单汇总

```
src/csharp/Atlas.ClientSDK/
├── Atlas.ClientSDK.csproj
├── AtlasClient.cs                       (Step 12.6)
├── LoginClient.cs                       (Step 12.3)
├── ClientEntityManager.cs               (Step 12.4)
├── ClientEntity.cs                      (Step 12.4)
├── AvatarFilter.cs                      (Step 12.5)
├── Network/
│   └── TcpTransport.cs                  (Step 12.1)
└── Protocol/
    ├── MessageCodec.cs                  (Step 12.2)
    ├── ClientMessages.cs                (Step 12.2)
    └── ServerMessages.cs                (Step 12.2)

src/csharp/Atlas.Generators.Entity/      (Step 12.7, 更新)
src/csharp/Atlas.Generators.Rpc/         (Step 12.7, 更新)

src/tools/atlas_test_client/             (Step 12.8)
├── atlas_test_client.csproj
└── Program.cs

tests/csharp/Atlas.ClientSDK.Tests/
├── TcpTransportTests.cs
├── LoginClientTests.cs
├── ClientEntityManagerTests.cs
├── AvatarFilterTests.cs
└── IntegrationTests.cs

tests/integration/
└── test_client_sdk.cpp
```

---

## 8. 依赖关系与执行顺序

```
Step 12.1: 网络传输层         ← 无依赖
Step 12.2: 消息编解码         ← 依赖 12.1 + Atlas.Shared
    │
Step 12.3: LoginClient        ← 依赖 12.2
Step 12.4: EntityManager      ← 依赖 12.2
Step 12.5: AvatarFilter       ← 无依赖 (纯算法)
    │
Step 12.6: AtlasClient        ← 依赖 12.3 + 12.4 + 12.5
Step 12.7: Source Generator    ← 可与 12.1-12.5 并行
    │
Step 12.8: test_client        ← 依赖 12.6 + 12.7
Step 12.9: 集成测试           ← 依赖全部
```

**推荐执行顺序:**

```
第 1 轮 (并行): 12.1 传输层 + 12.5 AvatarFilter + 12.7 Source Generator 客户端条件
第 2 轮:        12.2 消息编解码
第 3 轮 (并行): 12.3 LoginClient + 12.4 EntityManager
第 4 轮:        12.6 AtlasClient
第 5 轮:        12.8 test_client
第 6 轮:        12.9 集成测试
```

---

## 9. BigWorld 完整对照

| BigWorld | Atlas | 差异说明 |
|----------|-------|---------|
| C++ `ServerConnection` | C# `AtlasClient` | 纯 C#，Unity 原生 |
| UDP + TCP + WebSocket | TCP（初期） | 简化；后续可加 KCP |
| `IDAlias` (uint8) 带宽优化 | 每条消息携带 EntityID | TCP 带宽充裕 |
| 24 种 `avatarUpdate` 变体 | 单一 `EntityPositionUpdate` | 无需极致压缩 |
| Packed 位置 (26/51 bits) | 全精度 float×3 | TCP 不需要；后续可压缩 |
| `AvatarFilterHelper` (C++, 8 sample) | `AvatarFilter` (C#, 8 sample) | 算法对齐 |
| `selectEntity` 隐式状态 | 每消息显式 EntityID | 更简单、无状态 |
| `enterAoI` + `requestEntityUpdate` + `createEntity` | `EntityEnter` 一步完成 | 简化协议 |
| `CacheStamps` (EventNumber) | 初期不实现 | 后续优化 |
| C++ EntityManager | C# `ClientEntityManager` | 共享 Atlas.Shared |
| Python 客户端脚本 | C# Source Generator 代理 | 编译期类型安全 |
| `bandwidthNotification` 下行控制 | 服务端 Witness 控制 | 客户端无需限速 |
| RSA 登录加密 | 明文（初期）+ TLS | 现代方案 |

---

## 10. 关键设计决策记录

### 10.1 纯 C# 客户端（无 C++ SDK）

**决策: 只提供 C# SDK。**

Atlas 目标用户是 Unity 开发者。C# SDK 可直接用 NuGet 引用，IL2CPP AOT 兼容。
如果未来需要 Unreal/自定义引擎支持，可基于协议文档实现 C++ 客户端。

### 10.2 TCP 而非 UDP

**决策: 初期仅 TCP。**

BigWorld 用 UDP 是因为 2000 年代的网络环境和带宽极致优化需求。
现代环境中 TCP + Nagle 禁用足够低延迟。
后续可选加入 KCP（可靠 UDP）用于位置更新等对延迟敏感的场景。

### 10.3 协议简化（无 IDAlias / 无位置压缩）

**决策: 初期使用全精度、显式 EntityID 协议。**

BigWorld 的 IDAlias、PackedXYZ 等优化是为了 UDP MTU 限制（~1400 bytes）。
TCP 没有此限制，一个 EntityPositionUpdate ≈ 28 bytes，1000 实体位置更新 ≈ 28KB/tick，
在 10Hz 下 ≈ 280KB/s，完全可接受。

后续优化路径: 位置增量编码 → PackedXYZ → IDAlias。

### 10.4 AvatarFilter 延迟自适应

**保留 BigWorld 核心算法:**
- 环形缓冲区 8 个采样
- `idealLatency = latencyFrames × 采样间隔`
- 实际延迟以 `velocity × pow(|diff|, curvePower)` 速度向理想值收敛
- `curvePower = 2.0`（离理想值越远收敛越快）

这是客户端体验的关键——过低延迟导致抖动，过高延迟导致迟钝。

### 10.5 初期不实现的功能

| 功能 | 原因 | 何时实现 |
|------|------|---------|
| IDAlias 带宽优化 | TCP 不需要 | 如用 KCP 时 |
| 位置压缩 | TCP 带宽充裕 | 按需 |
| CacheStamps 重入优化 | 复杂度高，收益有限 | 按需 |
| 数据下载 (DataDownload) | 大文件传输场景少 | 按需 |
| Vehicle 位置变换 | 需 Vehicle 系统 | 按需 |
| 客户端预测 + 回滚 | FPS 类才需要 | 按需 |
| 断线重连 | 需要服务端状态保持 | 紧接本阶段 |
