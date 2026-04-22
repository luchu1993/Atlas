# 基于 .def 文件的进程感知代码生成 + C++ 安全校验

> 日期: 2026-04-14
> 状态: 已实现（C# Generator 部分完成，C++ 安全校验部分待实现）
> 关联: [BigWorld RPC 参考](../bigworld_ref/BIGWORLD_RPC_REFERENCE.md) | [Entity Mailbox 设计](../scripting/entity_mailbox_design.md) | [DefGenerator 统一重构](DEFGEN_CONSOLIDATION_DESIGN.md)

## 1. 背景

Atlas 对标 BigWorld 的分布式实体架构。当前缺少：

1. `.def` 定义文件作为实体的唯一真相来源
2. Source Generator 的进程上下文感知（Base/Cell/Client 生成不同代码）
3. C++ 层的客户端 RPC 接收、Exposed 方法校验、sourceEntityID 验证

本方案同时实现 Generator 和 C++ 安全校验链路。

---

## 2. .def XML 格式

每个实体类型一个 `.def` 文件，所有进程共享：

```xml
<entity name="Avatar">

  <properties>
    <property name="hp"       type="int32"   scope="all_clients"       persistent="true" />
    <property name="position" type="vector3" scope="all_clients" />
    <property name="gold"     type="int32"   scope="base"              persistent="true" />
    <property name="aiState"  type="int32"   scope="cell_public" />
    <property name="modelId"  type="int32"   scope="other_clients" />
    <property name="mana"     type="int32"   scope="cell_public_and_own" />
    <property name="secret"   type="string"  scope="base_and_client"   persistent="true" />
  </properties>

  <client_methods>
    <method name="ShowDamage">
      <arg name="amount" type="int32" />
      <arg name="attackerId" type="uint32" />
    </method>
  </client_methods>

  <cell_methods>
    <method name="CastSkill" exposed="own_client">
      <arg name="skillId" type="int32" />
      <arg name="targetId" type="uint32" />
    </method>
    <method name="OnEnterRegion">
      <arg name="regionId" type="int32" />
    </method>
  </cell_methods>

  <base_methods>
    <method name="UseItem" exposed="own_client">
      <arg name="itemId" type="int32" />
    </method>
    <method name="OnPlayerDead" />
  </base_methods>

</entity>
```

### 2.1 exposed 语义

| 值 | 含义 | 适用范围 |
|---|---|---|
| 省略 | 仅服务器内部可调用 | cell_methods, base_methods |
| `"own_client"` | 仅 owner 客户端可调用 | cell_methods, base_methods |
| `"all_clients"` | AoI 内任意客户端可调用 | **仅 cell_methods** |
| `"true"` | 等同于 `own_client`（简写） | cell_methods, base_methods |

> **与 BigWorld 的差异：** BigWorld 中裸 `<Exposed/>` 等于 `OWN_CLIENT + ALL_CLIENTS`（任何客户端可调）。
> Atlas 选择更保守的默认值：`exposed="true"` = `own_client`，需要 `all_clients` 时必须显式声明。
> 这减少了因遗漏而暴露方法给非 owner 客户端的风险。

#### 校验规则

| 规则 | 报错时机 | 说明 |
|---|---|---|
| `client_methods` 不允许 `exposed` | Generator 编译期 | 客户端方法由服务器调用，无需暴露 |
| `base_methods` 不允许 `exposed="all_clients"` | Generator 编译期 + C++ 加载时 | 架构约束：客户端只连一个 BaseApp，无法路由到其他实体的 Base |
| Exposed 方法参数不能含 Mailbox 类型 | Generator 编译期 | 客户端无法发送 Mailbox 引用（对应 BigWorld `CLIENT_UNUSABLE`） |

### 2.2 属性 scope（对齐 BigWorld Flags）

| scope | Ghost 同步 | Owner 客户端 | Other 客户端 | Base | BigWorld 等价 | 典型用途 |
|---|---|---|---|---|---|---|
| `cell_private` | - | - | - | - | `CELL_PRIVATE` | AI 内部状态 |
| `cell_public` | Yes | - | - | - | `CELL_PUBLIC` | 跨 Cell 可见但客户端不可见 |
| `own_client` | - | Yes | - | - | `OWN_CLIENT` | 背包、技能冷却 |
| `other_clients` | Yes | - | Yes | - | `OTHER_CLIENTS` | 模型、装备外观 |
| `all_clients` | Yes | Yes | Yes | - | `ALL_CLIENTS` | 血量、名字 |
| `cell_public_and_own` | Yes | - | Yes（注） | - | `CELL_PUBLIC_AND_OWN` | Ghost 可见 + owner 客户端 |
| `base` | - | - | - | Yes | `BASE` | 仅 Base 端数据 |
| `base_and_client` | - | Yes | - | Yes | `BASE_AND_CLIENT` | Base + owner 客户端 |

> **设计规律（继承自 BigWorld）：** 需要 Other 客户端看到的属性必然需要 Ghost 同步，
> 因为其他客户端的 Witness 可能运行在 Ghost 所在的 CellApp 上。

### 2.3 自动推断

- `has_cell`：有 `cell_methods` 或存在 `cell_private` / `cell_public` / `cell_public_and_own` 属性 → true
- `has_client`：有 `client_methods` 或存在 `own_client` / `all_clients` / `other_clients` / `base_and_client` 属性 → true

### 2.3 type_id

省略时按实体名字字母序自动分配。后续需要协议稳定性时可显式指定 `<entity name="Avatar" id="2">`。

---

## 3. 用户视角

```csharp
// ═══ CellApp 项目 ═══
[Entity("Avatar")]
public partial class Avatar : CellEntity
{
    // 从 .def 生成的 partial 方法，用户必须实现：
    public partial void CastSkill(int skillId, uint targetId)
    {
        Hp -= 50;
        AllClients.ShowDamage(50, EntityId);  // 生成的 client send stub
        if (Hp <= 0)
            Base.OnPlayerDead();              // 生成的 base send stub
    }

    public partial void OnEnterRegion(int regionId) { /* ... */ }
}
```

```csharp
// ═══ BaseApp 项目 ═══
[Entity("Avatar")]
public partial class Avatar : BaseEntity
{
    public partial void UseItem(int itemId) { Hp += 100; }
    public partial void OnPlayerDead() { Gold /= 2; }
}
```

```csharp
// ═══ Client 项目 ═══
[Entity("Avatar")]
public partial class Avatar : ClientEntity
{
    public partial void ShowDamage(int amount, uint attackerId) { /* UI */ }
}
```

---

## 4. RPC 角色矩阵

### 4.1 方法归属 x 进程上下文 → 生成行为

| 方法类型 | Base | Cell | Client (exposed) | Client (非 exposed) |
|---|---|---|---|---|
| `client_methods` | Send stub | Send stub（中继） | 用户实现 partial | 用户实现 partial |
| `cell_methods` | Send stub | 用户实现 partial | Send stub | Forbidden throw |
| `base_methods` | 用户实现 partial | Send stub | Send stub | Forbidden throw |

### 4.2 Mailbox 生成

| Mailbox | Base | Cell | Client |
|---|---|---|---|
| `Client` / `AllClients` / `OtherClients` | 生成 | 生成 | 不生成 |
| `Cell` | 生成 | 不生成 | 仅 exposed cell_methods |
| `Base` | 不生成 | 生成 | 仅 exposed base_methods |

### 4.3 Dispatcher 生成

| Dispatcher | Base | Cell | Client |
|---|---|---|---|
| `DispatchCellRpc` | 不生成 | 生成 | 不生成 |
| `DispatchBaseRpc` | 生成 | 不生成 | 不生成 |
| `DispatchClientRpc` | 不生成 | 不生成 | 生成 |

---

## 5. 实现步骤

### 第 1 步：创建 Atlas.Generators.Def 项目

**新建目录：** `src/csharp/Atlas.Generators.Def/`

```
Atlas.Generators.Def/
├── Atlas.Generators.Def.csproj    # netstandard2.0, Roslyn generator
├── DefParser.cs                   # XML 解析 → DefModel
├── DefModel.cs                    # 数据模型（EntityDefModel, MethodDefModel, PropertyDefModel）
├── ProcessContext.cs              # 进程上下文枚举 + RPC 角色矩阵
├── DefGenerator.cs                # IIncrementalGenerator 主入口
├── DefTypeHelper.cs               # .def 类型 ↔ C# 类型/Writer/Reader 映射
├── DefDiagnosticDescriptors.cs    # 诊断描述
└── Emitters/
    ├── PropertiesEmitter.cs       # 字段声明 + Property + DirtyFlags + On{Name}Changed 回调
    ├── SerializationEmitter.cs    # Serialize / Deserialize (version+fieldCount+bodyLength)
    ├── DeltaSyncEmitter.cs        # Delta/Owner/Other client 同步
    ├── RpcStubEmitter.cs          # Send stub / Forbidden stub
    ├── MailboxEmitter.cs          # Mailbox struct
    ├── DispatcherEmitter.cs       # RPC 分发
    ├── FactoryEmitter.cs          # EntityFactory.Create() / CreateByTypeId()
    ├── TypeRegistryEmitter.cs     # DefEntityTypeRegistry ([ModuleInitializer])
    └── RpcIdEmitter.cs            # RpcIds 常量
```

> **注意**: 原设计中 PropertyEmitter/SerializationEmitter/DeltaSyncEmitter/FactoryEmitter 计划从一开始就包含在 DefGenerator 中。
> 实际实施分两步：先实现 RPC 相关 Emitter，后在 [DefGenerator 统一重构](DEFGEN_CONSOLIDATION_DESIGN.md) 中补充属性/序列化相关 Emitter 并删除了旧的 EntityGenerator 和 RpcGenerator。

#### DefParser.cs

用 `System.Xml.Linq`（netstandard2.0 可用）解析 `.def`：

```csharp
internal static class DefParser
{
    public static EntityDefModel? Parse(SourceText text)
    {
        var doc = XDocument.Parse(text.ToString());
        var root = doc.Root; // <entity>
        // 解析 properties, client_methods, cell_methods, base_methods
        // 返回 EntityDefModel
    }
}
```

#### DefModel.cs

```csharp
internal sealed class EntityDefModel
{
    public string Name { get; set; }
    public int? ExplicitTypeId { get; set; }
    public List<PropertyDefModel> Properties { get; }
    public List<MethodDefModel> ClientMethods { get; }
    public List<MethodDefModel> CellMethods { get; }
    public List<MethodDefModel> BaseMethods { get; }
    public bool HasCell { get; }   // 从内容推断
    public bool HasClient { get; } // 从内容推断
}

internal sealed class MethodDefModel
{
    public string Name { get; set; }
    public ExposedScope Exposed { get; set; } // None / OwnClient / AllClients
    public List<ArgDefModel> Args { get; }
}

internal enum ExposedScope { None, OwnClient, AllClients }
```

#### ProcessContext.cs

```csharp
internal enum ProcessContext { Server, Base, Cell, Client }
internal enum RpcRole { Send, Receive, Forbidden }

internal static class RpcRoleHelper
{
    public static RpcRole GetRole(string section, ProcessContext ctx, ExposedScope exposed)
    {
        return (section, ctx) switch
        {
            ("client_methods", ProcessContext.Client) => RpcRole.Receive,
            ("client_methods", _)                     => RpcRole.Send,

            ("cell_methods", ProcessContext.Cell)      => RpcRole.Receive,
            ("cell_methods", ProcessContext.Base)      => RpcRole.Send,
            ("cell_methods", ProcessContext.Client)    => exposed != ExposedScope.None
                                                           ? RpcRole.Send : RpcRole.Forbidden,

            ("base_methods", ProcessContext.Base)      => RpcRole.Receive,
            ("base_methods", ProcessContext.Cell)      => RpcRole.Send,
            ("base_methods", ProcessContext.Client)    => exposed != ExposedScope.None
                                                           ? RpcRole.Send : RpcRole.Forbidden,

            (_, ProcessContext.Server)                 => RpcRole.Send,

            _ => RpcRole.Forbidden,
        };
    }
}
```

#### DefGenerator.cs

```csharp
public void Initialize(IncrementalGeneratorInitializationContext context)
{
    // 1. 读取 .def 文件（AdditionalTextsProvider）
    var defs = context.AdditionalTextsProvider
        .Where(static f => f.Path.EndsWith(".def"))
        .Select(static (f, ct) => DefParser.Parse(f.GetText(ct)!))
        .Where(static m => m is not null);

    // 2. 读取进程上下文（预处理器符号）
    var processCtx = context.CompilationProvider
        .Select(static (c, _) =>
        {
            var syms = c.Options.PreprocessorSymbolNames;
            if (syms.Contains("ATLAS_BASE"))   return ProcessContext.Base;
            if (syms.Contains("ATLAS_CELL"))   return ProcessContext.Cell;
            if (syms.Contains("ATLAS_CLIENT")) return ProcessContext.Client;
            return ProcessContext.Server;
        });

    // 3. 查找用户的 [Entity("Name")] 类
    var userEntities = context.SyntaxProvider
        .ForAttributeWithMetadataName("Atlas.Entity.EntityAttribute", ...)
        .Select(static (ctx, _) => /* className, namespace, typeName */);

    // 4. 合并 → 生成
    var combined = defs.Collect()
        .Combine(userEntities.Collect())
        .Combine(processCtx);
    context.RegisterSourceOutput(combined, Execute);
}
```

匹配逻辑：`[Entity("Avatar")]` 的 typeName 与 `.def` 的 `name="Avatar"` 匹配。只为当前编译中有 `[Entity]` 类的实体生成代码。

#### RpcStubEmitter.cs（核心）

```csharp
static void EmitMethod(StringBuilder sb, MethodDefModel method, RpcRole role, ...)
{
    switch (role)
    {
        case RpcRole.Receive:
            // partial 方法声明，用户必须实现
            sb.AppendLine($"    public partial void {method.Name}({paramList});");
            break;

        case RpcRole.Send:
            // 序列化 + 发送 stub
            sb.AppendLine($"    public void {method.Name}({paramList})");
            sb.AppendLine("    {");
            // ... SpanWriter 序列化 + Send{Direction}Rpc(...)
            sb.AppendLine("    }");
            break;

        case RpcRole.Forbidden:
            // 抛异常 stub
            sb.AppendLine($"    public void {method.Name}({paramList})");
            sb.AppendLine("    {");
            sb.AppendLine($"        throw new InvalidOperationException(\"...\");");
            sb.AppendLine("    }");
            break;
    }
}
```

#### DispatcherEmitter.cs

只在 Receive 方向生成，直接调用 partial 方法（不用 On 前缀）：

```csharp
target.CastSkill(skillId, targetId);  // 直接调用用户的 partial 实现
```

### 第 2 步：创建 .def 示例文件

**文件：** `entity_defs/Avatar.def`, `entity_defs/Account.def`

### 第 3 步：添加进程基类（三包隔离）

每个进程一个独立运行时包，互不引用，共同依赖 `Atlas.Shared`：

```
Atlas.Shared          ← 序列化、协议常量、属性定义、Log、ThreadGuard（各进程共享）
  ├── Atlas.Base      ← BaseApp 运行时：BaseEntity + NativeApi（Base 侧）
  ├── Atlas.Cell      ← CellApp 运行时：CellEntity + NativeApi（Cell 侧）
  └── Atlas.Client    ← 客户端运行时：ClientEntity + ClientNativeApi（Unity 可用）
```

#### 从 Runtime 移入 Shared 的文件

以下文件移入 `Atlas.Shared`，需解耦 native 依赖：

**Log.cs** — 移入 Shared，将 `NativeApi.LogMessage` 替换为可注册的委托：

```csharp
// Atlas.Shared/Log/Log.cs
public static class Log
{
    // 各进程启动时注册自己的 sink
    public delegate void LogSink(int level, ReadOnlySpan<byte> utf8Message);
    private static LogSink? s_sink;

    public static void SetSink(LogSink sink) => s_sink = sink;

    // 公共 API 不变
    public static void Trace(string message) => Send(0, message);
    public static void Debug(string message) => Send(1, message);
    public static void Info(string message)  => Send(2, message);
    // ...

    private static void Send(int level, string message)
    {
        // 编码逻辑不变，最后调用 s_sink 而非 NativeApi
        s_sink?.Invoke(level, buf[..written]);
    }
}
```

各进程启动时注册：
- **服务器 (Runtime):** `Log.SetSink(NativeApi.LogMessage)` — 转发到 C++ 日志系统
- **客户端 (Unity):** `Log.SetSink((level, msg) => UnityEngine.Debug.Log(...))` — 转发到 Unity

**其他零改动直接移入 Shared 的文件：**
- `ThreadGuard.cs` — 纯 `Environment.CurrentManagedThreadId`
- `DeltaHistory.cs` — 纯数据结构
- `ClientReplicationState.cs` — 纯数据容器
- `EngineInfo.cs` — 版本常量

**新建项目：**
- `src/csharp/Atlas.Base/` → `BaseEntity`
- `src/csharp/Atlas.Cell/` → `CellEntity`
- `src/csharp/Atlas.Client/` → `ClientEntity`

```csharp
// Atlas.Base (BaseApp 进程)
public abstract class BaseEntity
{
    public uint EntityId { get; internal set; }
    public abstract string TypeName { get; }
    public abstract void Serialize(ref SpanWriter writer);
    public abstract void Deserialize(ref SpanReader reader);
    protected internal virtual void OnInit(bool isReload) { }
    protected internal virtual void OnTick(float deltaTime) { }
    protected internal virtual void OnDestroy() { }

    protected internal void SendClientRpc(int rpcId, ReadOnlySpan<byte> payload)
        => NativeApi.SendClientRpc(EntityId, (uint)rpcId, payload);
    protected internal void SendCellRpc(int rpcId, ReadOnlySpan<byte> payload)
        => NativeApi.SendCellRpc(EntityId, (uint)rpcId, payload);
}

// Atlas.Cell (CellApp 进程)
public abstract class CellEntity
{
    public uint EntityId { get; internal set; }
    public abstract string TypeName { get; }
    public abstract void Serialize(ref SpanWriter writer);
    public abstract void Deserialize(ref SpanReader reader);
    protected internal virtual void OnInit(bool isReload) { }
    protected internal virtual void OnTick(float deltaTime) { }
    protected internal virtual void OnDestroy() { }

    protected internal void SendClientRpc(int rpcId, ReadOnlySpan<byte> payload)
        => NativeApi.SendClientRpc(EntityId, (uint)rpcId, payload);
    protected internal void SendBaseRpc(int rpcId, ReadOnlySpan<byte> payload)
        => NativeApi.SendBaseRpc(EntityId, (uint)rpcId, payload);
}

// Atlas.Client (客户端进程，不依赖服务器)
public abstract class ClientEntity
{
    public uint EntityId { get; internal set; }
    public abstract string TypeName { get; }

    // 客户端仅发送 exposed 的 base/cell RPC
    protected internal void SendCellRpc(int rpcId, ReadOnlySpan<byte> payload)
        => ClientNativeApi.SendCellRpc(EntityId, (uint)rpcId, payload);
    protected internal void SendBaseRpc(int rpcId, ReadOnlySpan<byte> payload)
        => ClientNativeApi.SendBaseRpc(EntityId, (uint)rpcId, payload);
}
```

注意：`BaseEntity` 没有 `SendBaseRpc`（不会给自己发），`CellEntity` 没有 `SendCellRpc`（同理）。

### 第 4 步：各进程 NativeApi

每个包自带各自的 NativeApi，只暴露该进程需要的 P/Invoke：

- `Atlas.Base/NativeApi.cs` → `SendClientRpc`, `SendCellRpc`
- `Atlas.Cell/NativeApi.cs` → `SendClientRpc`, `SendBaseRpc`
- `Atlas.Client/ClientNativeApi.cs` → `SendCellRpc`, `SendBaseRpc`（通过客户端网络层）

原 `Atlas.Runtime/Core/NativeApi.cs` 保留作为 Server 模式（向后兼容，包含所有方法）。

### 第 5 步：C++ .def 解析与 EntityDefRegistry 扩展

**修改 `entity_type_descriptor.hpp`：**

```cpp
enum class ExposedScope : uint8_t {
    None       = 0,
    OwnClient  = 1,
    AllClients = 2,
};

struct RpcDescriptor {
    std::string name;
    uint32_t rpc_id;
    std::vector<PropertyDataType> param_types;
    ExposedScope exposed = ExposedScope::None;  // 新增
    // ...
};
```

**新建 `def_file_parser.hpp/cpp`：** XML 解析器，用 tinyxml2 解析 `.def` 文件。

**修改 `entity_def_registry.hpp/cpp`：**

```cpp
void load_from_def_directory(const std::filesystem::path& def_dir);
const RpcDescriptor* find_rpc(uint32_t rpc_id) const;
bool is_exposed(uint32_t rpc_id) const;
ExposedScope get_exposed_scope(uint32_t rpc_id) const;
```

### 第 6 步：BaseApp 外部接口 — 客户端 RPC 接收

**修改 `baseapp_messages.hpp`：**

```cpp
constexpr MessageID kClientBaseRpc = 2022;
constexpr MessageID kClientCellRpc = 2023;
```

**修改 `baseapp.cpp`：** 注册处理器 `on_client_base_rpc`, `on_client_cell_rpc`

### 第 7 步：BaseApp — Exposed 校验 + Proxy 路由

```cpp
void BaseApp::on_client_base_rpc(Channel& channel, MessageReader& reader)
{
    auto* proxy = find_proxy_by_channel(channel);
    if (!proxy) return;

    uint32_t rpc_id = reader.read_uint32();

    auto* rpc_desc = entity_def_registry_.find_rpc(rpc_id);
    if (!rpc_desc || rpc_desc->exposed == ExposedScope::None)
    {
        ATLAS_LOG_WARNING("Client tried to call non-exposed base method");
        return;
    }

    // OWN_CLIENT: Proxy 路由天然满足
    auto payload = reader.read_remaining();
    script_engine().dispatch_base_rpc(proxy->entity_id(), rpc_id, payload);
}
```

### 第 8 步：BaseApp — Client→Cell 转发 + sourceEntityID 嵌入

客户端消息格式：`[target_entity_id | rpc_id | payload]`

- `target_entity_id == proxy.entity_id()` → 调用自己的 Cell 方法（OWN_CLIENT / ALL_CLIENTS）
- `target_entity_id != proxy.entity_id()` → 调用其他实体的 Cell 方法（仅 ALL_CLIENTS）

```cpp
void BaseApp::on_client_cell_rpc(Channel& channel, MessageReader& reader)
{
    auto* proxy = find_proxy_by_channel(channel);
    if (!proxy) return;

    uint32_t target_entity_id = reader.read_uint32();
    uint32_t rpc_id = reader.read_uint32();

    auto* rpc_desc = entity_def_registry_.find_rpc(rpc_id);
    if (!rpc_desc || rpc_desc->exposed == ExposedScope::None)
        return;

    // 跨实体调用：必须是 ALL_CLIENTS 方法
    if (target_entity_id != proxy->entity_id()
        && rpc_desc->exposed != ExposedScope::AllClients)
    {
        ATLAS_LOG_WARNING("Client tried to call OWN_CLIENT cell method on other entity");
        return;
    }

    auto* cell_channel = resolve_cell_channel(target_entity_id);
    if (!cell_channel) return;

    // 嵌入 sourceEntityID（= proxy.entity_id()，客户端无法伪造）
    auto payload = reader.read_remaining();
    cell_channel->send_message(CellAppMessages::kClientCellRpcForward,
        target_entity_id,         // 目标实体
        proxy->entity_id(),       // sourceEntityID
        rpc_id, payload);
}
```

### 第 9 步：CellApp — sourceEntityID 验证

```cpp
void CellApp::on_client_cell_rpc_forward(Channel& channel, MessageReader& reader)
{
    uint32_t target_entity_id = reader.read_uint32();
    uint32_t source_entity_id = reader.read_uint32();  // BaseApp 嵌入的
    uint32_t rpc_id = reader.read_uint32();
    auto payload = reader.read_remaining();

    auto* entity = entity_manager_.find(target_entity_id);
    if (!entity) return;

    // Ghost 透明转发（Phase 11）：
    // if (!entity->is_real()) { entity->forward_to_real(...); return; }

    auto* rpc_desc = entity_def_registry_.find_rpc(rpc_id);
    if (rpc_desc->exposed == ExposedScope::OwnClient)
    {
        if (target_entity_id != source_entity_id)
        {
            ATLAS_LOG_WARNING("Blocked cell RPC: source {} != target {}",
                              source_entity_id, target_entity_id);
            return;  // 拒绝：不是 owner
        }
    }
    // ALL_CLIENTS: 不验证 sourceEntityID（设计如此）

    script_engine().dispatch_cell_rpc(target_entity_id, rpc_id, payload);
}
```

### 第 10 步：C++ native API 扩展

**修改 `clr_native_api_defs.hpp`：**

```cpp
X(void, send_server_rpc,
    (uint32_t entity_id, uint32_t rpc_id,
     const uint8_t* payload, int32_t len),
    atlas::get_native_api_provider().send_server_rpc(entity_id, rpc_id, ...))
```

**修改 `base_native_provider.hpp/cpp`：** 添加 `send_server_rpc` 默认实现。

### 第 11 步：单元测试

**C# Generator 测试 (`DefGeneratorTests.cs`)：**

1. 解析 .def XML → 正确的 DefModel（含 8 种 scope）
2. Base 上下文 → base_methods partial 声明 + cell/client send stub
3. Cell 上下文 → cell_methods partial 声明 + base/client send stub
4. Client 上下文 → client_methods partial 声明 + exposed send stub + 非 exposed forbidden
5. RPC ID 全进程一致
6. Dispatcher 调用 `target.Method()`（非 On 前缀）
7. `client_methods` 标记 `exposed` → 报 ATLAS_DEF002
8. `base_methods` 使用 `exposed="all_clients"` → 报 ATLAS_DEF003
9. Client 上下文不生成 Base mailbox 的跨实体调用

**C++ 测试 (`test_def_file_parser.cpp`)：**

1. 解析 .def XML → EntityTypeDescriptor
2. Exposed 标记正确读取
3. RPC 查找和校验函数

**C++ 测试 (`test_client_rpc_validation.cpp`)：**

1. 非 exposed RPC → 拒绝
2. Exposed OWN_CLIENT → 接受 owner 调用
3. Exposed OWN_CLIENT → 拒绝非 owner 调用
4. Exposed ALL_CLIENTS → 接受任意调用
5. 跨实体 Cell RPC + OWN_CLIENT → BaseApp 拒绝（target != source）
6. 跨实体 Cell RPC + ALL_CLIENTS → BaseApp 接受并转发
7. `base_methods` + `ALL_CLIENTS` → .def 加载时拒绝

---

## 6. 安全校验全链路

### 6.1 Client → Base（自己的 Base）

```
Client                    BaseApp
  |                         |
  |-- ClientBaseRpc ------> |
  |   (rpc_id + payload)    |
  |                         +- 1. find_proxy_by_channel()
  |                         +- 2. find_rpc(rpc_id)
  |                         +- 3. is_exposed? -> No -> DROP
  |                         +- 4. Proxy 路由隔离 (天然: 只能调自己的 Base)
  |                         +- 5. dispatch to C#
```

> 客户端不能调用其他实体的 Base 方法（与 BigWorld 一致，Section 5.1-5.3）。
> Generator 在客户端不生成 Base mailbox 的跨实体调用。

### 6.2 Client → Cell（自己或其他实体）

```
Client                    BaseApp                         CellApp
  |                         |                               |
  |-- ClientCellRpc ------> |                               |
  |   (target + rpc_id      |                               |
  |    + payload)            |                               |
  |                         +- 1. find_proxy_by_channel()   |
  |                         +- 2. find_rpc(rpc_id)          |
  |                         +- 3. is_exposed? -> No -> DROP |
  |                         +- 4. target != self &&          |
  |                         |     !ALL_CLIENTS? -> DROP     |
  |                         +- 5. embed sourceEntityID      |
  |                         |      (= proxy.entity_id)      |
  |                         +--- forward ------------------> |
  |                         |                               +- 6. find entity
  |                         |                               +- 7. Ghost? -> forward to Real (Phase 11)
  |                         |                               +- 8. OWN_CLIENT?
  |                         |                               |     source == target?
  |                         |                               |     No -> DROP
  |                         |                               +- 9. dispatch to C#
```

### 6.3 可达性矩阵（对齐 BigWorld Section 5.1）

| 路径 | 允许？ | 条件 |
|---|---|---|
| Client → 自己的 Base | Yes | exposed = own_client / true |
| Client → 其他实体 Base | **No** | 架构禁止（无路由路径） |
| Client → 自己的 Cell | Yes | exposed = own_client / all_clients / true |
| Client → 其他实体 Cell | Yes | exposed = all_clients（仅此值） |

---

## 7. 文件清单

### 新建文件

| 文件 | 说明 |
|---|---|
| `src/csharp/Atlas.Generators.Def/` (整个目录) | 新 Generator 项目 |
| `src/lib/entitydef/def_file_parser.hpp/cpp` | C++ .def XML 解析器 |
| `src/csharp/Atlas.Base/` | BaseApp 运行时：BaseEntity + NativeApi |
| `src/csharp/Atlas.Cell/` | CellApp 运行时：CellEntity + NativeApi |
| `src/csharp/Atlas.Client/` | 客户端运行时：ClientEntity + ClientNativeApi |
| `entity_defs/Avatar.def` | 示例定义文件 |
| `entity_defs/Account.def` | 示例定义文件 |
| `tests/csharp/Atlas.Generators.Tests/DefGeneratorTests.cs` | Generator 测试 |
| `tests/unit/entitydef/test_def_file_parser.cpp` | C++ 解析测试 |
| `tests/unit/baseapp/test_client_rpc_validation.cpp` | C++ 校验测试 |

### 修改文件

| 文件 | 改动 |
|---|---|
| `src/lib/entitydef/entity_type_descriptor.hpp` | RpcDescriptor 增加 ExposedScope |
| `src/lib/entitydef/entity_def_registry.hpp/cpp` | 添加 .def 加载 + RPC 查找 |
| `src/server/baseapp/baseapp_messages.hpp` | 注册客户端 RPC 消息 ID |
| `src/server/baseapp/baseapp.cpp` | 外部接口 RPC 处理器 + Exposed 校验 |
| `src/server/cellapp/cellapp.cpp` | sourceEntityID 验证 |
| `src/lib/clrscript/clr_native_api_defs.hpp` | 添加 send_server_rpc |
| `src/lib/clrscript/base_native_provider.hpp/cpp` | send_server_rpc 默认实现 |
| `src/csharp/Atlas.Runtime/Atlas.Runtime.csproj` | 引用新 Generator（向后兼容，Server 模式） |

---

## 8. 验证

1. `dotnet build src/csharp/Atlas.Generators.Def/` — 构建 Generator
2. `dotnet test tests/csharp/Atlas.Generators.Tests/` — Generator 测试
3. `cmake --build --preset debug-windows` — C++ 构建
4. `ctest --preset debug-windows` — C++ 测试
5. `dotnet build src/csharp/Atlas.Runtime/` — 默认上下文，向后兼容
6. 检查 `obj/Debug/net9.0/generated/` 下的 `.g.cs` 文件

---

## 9. 补充：对齐 BigWorld 的额外机制

### 9.1 RPC 频率限制（BigWorld 缺失，Atlas 可新增）

BigWorld 没有 RPC rate limiting（参考文档 Section 4.6）。恶意客户端可高频发送 exposed RPC。
Atlas 可在 C++ BaseApp 校验层新增可选的频率限制：

```cpp
// baseapp.cpp — 在 exposed 校验通过后
if (!rate_limiter_.allow(proxy->entity_id(), rpc_id))
{
    ATLAS_LOG_WARNING("RPC rate limit exceeded");
    return;
}
```

建议实现方式：令牌桶或滑动窗口，按 (entity_id, rpc_id) 维度限流。
可在 `.def` 中配置：`<method name="CastSkill" exposed="own_client" rate_limit="10/s">`。
此功能为可选增强，不阻塞核心实现。

### 9.2 Ghost 透明转发（Phase 11 预留）

BigWorld 通过 `REAL_ONLY` 消息标记实现 Ghost → Real 透明转发（参考文档 Section 6）。
当前设计在 CellApp 第 9 步中预留了注释占位：

```cpp
// Ghost 透明转发（Phase 11）：
// if (!entity->is_real()) { entity->forward_to_real(...); return; }
```

`.def` 格式无需 Ghost 专有配置（与 BigWorld 一致）。属性的 `scope` 已隐含 Ghost 同步需求：
- 含 `other_clients` / `all_clients` / `cell_public` / `cell_public_and_own` → 需要 Ghost 同步
- 含 `own_client` / `base` / `cell_private` → 不需要 Ghost 同步

### 9.3 Generator 诊断规则汇总

| 诊断 ID | 严重度 | 规则 |
|---|---|---|
| ATLAS_DEF001 | Error | `[Entity("X")]` 找不到匹配的 `.def` 文件 |
| ATLAS_DEF002 | Error | `client_methods` 标记了 `exposed`（不允许） |
| ATLAS_DEF003 | Error | `base_methods` 使用了 `exposed="all_clients"`（架构禁止） |
| ATLAS_DEF004 | Error | Exposed 方法参数含不可序列化类型 |
| ATLAS_DEF005 | Warning | `partial class` 缺少 `[Entity]` 匹配的 partial 方法实现 |
| ATLAS_DEF006 | Error | `.def` XML 解析失败 |
| ATLAS_DEF007 | Error | 重复的 type_id |

---

## 10. ProcessContext.Client 模式：实测产物与缺口（2026-04-21）

§4 的 RPC 角色矩阵、§5 的 Emitter 设计都对"Client 上下文该产出什么"做了规约，但 Generator 的 Client 分支上线后一直没被对照 BigWorld 做过一轮完整审计。本节是实测 + 对照修订：用 `samples/client/Atlas.ClientSample.csproj`（`<DefineConstants>ATLAS_CLIENT</DefineConstants>`，引用 `Atlas.Client` + `Atlas.Generators.Def` as Analyzer）+ `entity_defs/Avatar.def` 触发 Generator，产物落在 `samples/client/obj/generated/Atlas.Generators.Def/Atlas.Generators.Def.DefGenerator/`。

### 10.1 实测已覆盖的产物

| 文件 | 核心内容 | 状态 |
|---|---|---|
| `Avatar.Properties.g.cs` | backing field + 公开 get/set + `OnXxxChanged` partial 声明 | 正常，但 setter 有缺陷（见 §10.2）|
| `Avatar.RpcStubs.g.cs` | `client_methods` → `public partial void X(...)` 接收声明；exposed `cell_methods` / `base_methods` → `SendCellRpc` / `SendBaseRpc` 发送 stub；非 exposed → `throw InvalidOperationException` | 符合 §4.1 矩阵 |
| `Avatar.Serialization.g.cs` | `TypeName` override + `Deserialize(ref SpanReader)`（version + fieldCount + bodyLength 容错跳过）；不生成 `Serialize` | 正常，但直写字段绕过 setter（见 §10.2）|
| `Avatar.Mailboxes.g.cs` | `AvatarBaseMailbox`（只含 exposed base_methods）+ `AvatarCellMailbox`（只含 exposed cell_methods）；无 `AvatarClientMailbox` | 符合 §4.2 |
| `DefRpcDispatcher.g.cs` | `[ModuleInitializer]` 注册 `Atlas.Client.ClientCallbacks.ClientRpcDispatcher` → switch 到 `target.Method(...)` | 接收派发已就绪 |
| `EntityFactory.g.cs` | `[ModuleInitializer]` 调 `Atlas.Client.ClientEntityFactory.Register(typeId, factory)` | 正常 |
| `DefEntityTypeRegistry.g.cs` | 通过 `Atlas.Client.ClientEntityRegistryBridge.RegisterEntityType` 注册 | 正常 |
| `RpcIds.g.cs` | 全进程一致的 RPC ID 常量 | 正常 |

**显著缺失**：对比服务端（`samples/stress/Atlas.StressTest.Cell/obj/generated/...`），客户端**不生成 `*.DeltaSync.g.cs`**。`DeltaSyncEmitter.cs:19-20` 显式短路：`if (ctx == ProcessContext.Client) return null;`

### 10.2 与 BigWorld 对照发现的缺口

对照 BigWorld `common/simple_client_entity.cpp:135-163` 的 `propertyEvent()` 与 `client/bw_entity.cpp:624-653` 的 `onPositionUpdated()`（完整分析见 `PROPERTY_SYNC_DESIGN.md §9`），当前 Client 模式产物存在四项语义偏差：

| # | 缺口 | 现状代码位置 | 需修正 |
|---|---|---|---|
| A | **客户端无 delta 解码** | `DeltaSyncEmitter.cs:19-20` 直接返回 null | 对称发射 `ApplyReplicatedDelta(ref SpanReader)`：按 `_dirtyFlags` bitmap 读字段，逐字段写入 + 触发回调。客户端**不维护** `_dirtyFlags` 字段，bitmap 只作一次性解码分发 |
| B | **setter 不触发 `OnXxxChanged`** | `PropertiesEmitter.cs:111-140` 的 `EmitProperty`：client 分支（`withDirtyTracking=false`）只产出 `set => _field = value;` | client 分支也生成 `if (old != new) { var o = _field; _field = value; OnXxxChanged(o, value); }`，但**不设置 dirty bit**（客户端没有写端诉求）|
| C | **`Deserialize` 直写字段绕过 setter** | `SerializationEmitter` 客户端产物用 `_hp = reader.ReadInt32();` 等 | **保持现状**——对齐 BigWorld `shouldUseCallback=false` 的初始快照语义（enter-AoI 时回调不应触发）。但需在 `ClientEntity` 加 `OnEnterWorld()` 虚方法，框架在快照应用完毕后统一调一次，供脚本做整体初始化 |
| D | **Position 回调语义错位** | Position 当前在 `Avatar.Properties.g.cs` 里生成 `OnPositionChanged(old, new)` partial，与其它属性同构 | Position 在客户端接收端**不应**走属性回调链：C++ 宿主将 `0xF003` 信封的 `kEntityPositionUpdate` kind 桥到 `ClientEntity.ApplyPositionUpdate(Vector3)`；该函数直写 `_position`、调 `OnPositionUpdated(Vector3 newPos)` 专用钩子（对齐 BigWorld `onPositionUpdated()`）。Generator 层应在 `ApplyReplicatedDelta` bitmap 中**跳过** Position 位，或发诊断警告 |

### 10.3 `ClientEntity` 基类需补的入口

客户端需要的运行时钩子，Generator 生成的代码会调用：

```csharp
public abstract class ClientEntity  // src/csharp/Atlas.Client/ClientEntity.cs
{
    // 已有：EntityId, TypeName, Deserialize, OnInit, OnDestroy, SendCellRpc, SendBaseRpc

    // 新增（本节）：
    protected internal virtual void OnEnterWorld() { }
    // 框架在 Deserialize (enter-AoI 初始快照) 应用完毕后统一调用一次。
    // 对齐 BigWorld 的 shouldUseCallback=false 路径：初始属性不触发 OnXxxChanged，
    // 脚本在 OnEnterWorld 里基于"已完整同步的当前状态"做整体初始化。

    public void ApplyPositionUpdate(Vector3 newPos) { /* 由 Generator 或手写填入 */ }
    // 专用于 kEntityPositionUpdate volatile 通道。直写 _position，调 OnPositionUpdated。
    // 不触发 OnPositionChanged（避免与属性回调链混淆）。

    protected internal virtual void OnPositionUpdated(Vector3 newPos) { }
    // 客户端脚本的位置钩子。对齐 BigWorld BWEntity::onPositionUpdated()。
}
```

`ApplyPositionUpdate` 可以由基类手写（Position 是所有实体通用的字段），也可以由 Generator 为有 Position 属性的实体发射；后者更安全，避免不同实体 Position 字段布局偏差。

### 10.4 Emitter 层任务清单

| # | 任务 | 文件 |
|---|---|---|
| 10.1 | `DeltaSyncEmitter` 增加 Client 分支，对称发射 `ApplyReplicatedDelta` | `Emitters/DeltaSyncEmitter.cs` |
| 10.2 | `PropertiesEmitter` Client 分支 setter 改为触发 `OnXxxChanged`（不设 dirty bit） | `Emitters/PropertiesEmitter.cs:111-140` |
| 10.3 | `ClientEntity` 增 `OnEnterWorld`、`ApplyPositionUpdate`、`OnPositionUpdated` | `src/csharp/Atlas.Client/ClientEntity.cs` |
| 10.4 | `SerializationEmitter` 客户端 `Deserialize` 末尾调 `OnEnterWorld()` | `Emitters/SerializationEmitter.cs` |
| 10.5 | Generator 诊断：Position 属性出现在 `ApplyReplicatedDelta` 位图时报 `ATLAS_DEF008`（或自动跳过该位）| `DefDiagnosticDescriptors.cs`，见 §10.5 |
| 10.6 | 测试：`DefGeneratorTests.cs` 新增 Client 上下文 `ApplyReplicatedDelta` 生成 + setter 回调 + Deserialize 不触发回调的用例 | `tests/csharp/Atlas.Generators.Tests/` |
| 10.7 | 验收：`dotnet build samples/client/` 后产物目录多出 `Avatar.DeltaSync.g.cs` 且只含 `ApplyReplicatedDelta` | 手动验证 |

### 10.5 新增诊断

| 诊断 ID | 严重度 | 规则 |
|---|---|---|
| ATLAS_DEF008 | Warning | Client 上下文下，`.def` 中名为 `position` 的属性若参与 `ApplyReplicatedDelta` bitmap，将与 `kEntityPositionUpdate` volatile 通道产生双重触发。Generator 自动从 bitmap 中跳过 Position 位；如需保留按属性同步，显式加 `<property name="position" ... volatile="false" />` 标记 |

（`volatile` 属性标记是后续工作，当前先用实体名约定 `position` 做兜底。）

### 10.6 与 PROPERTY_SYNC_DESIGN 的交叉引用

- 本节任务 10.1 对应 `PROPERTY_SYNC_DESIGN.md §9.5` 任务 9.1
- 本节任务 10.2 对应 `PROPERTY_SYNC_DESIGN.md §9.5` 任务 9.2
- 本节任务 10.3 对应 `PROPERTY_SYNC_DESIGN.md §9.5` 任务 9.3 + 9.4
- 本节任务 10.5 对应 `PROPERTY_SYNC_DESIGN.md §9.5` 任务 9.6

两份文档视角不同：`PROPERTY_SYNC_DESIGN.md §9` 从"属性同步整体链路"切入，强调 BigWorld 双通道与 Atlas 服务端/客户端两侧的对称；本节从"Generator 实际发射什么"切入，具体到每个 Emitter 的改动点。落地时以 `PROPERTY_SYNC_DESIGN.md §9.5` 的任务编号为主索引。
