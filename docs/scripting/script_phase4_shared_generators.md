# ScriptPhase 4: 共享程序集 + Source Generator 体系

> 预估周期: 3–4 周 | 前置依赖: ScriptPhase 3 完成 | **状态: 🟡 进行中**

> **架构演进**：原设计中的 `Atlas.Generators.Entity` + `Atlas.Generators.Rpc` 已被统一合并入 `Atlas.Generators.Def`，改由 `.def` 文件驱动代码生成；`[Replicated]`、`[Persistent]`、`[ServerOnly]`、`[ClientRpc]`、`[CellRpc]`、`[BaseRpc]` 等 C# Attribute 已删除。`Atlas.Generators.Events` 仍作为独立 Generator 存在。完整演进背景见 [DEFGEN_CONSOLIDATION_DESIGN.md](../DEFGEN_CONSOLIDATION_DESIGN.md) 与 [DEF_GENERATOR_DESIGN.md](../DEF_GENERATOR_DESIGN.md)。

---

## 目标

1. 建立 `Atlas.Shared` 共享程序集（`netstandard2.1`），可在服务端和 Unity IL2CPP 上运行。
2. 基于 `.def` 文件 + `Atlas.Generators.Def` 生成实体属性/序列化/RPC stub/Mailbox/Dispatcher/工厂/类型注册代码。
3. `Atlas.Generators.Events` 生成事件订阅注册/注销代码。
4. 所有生成的代码 100% IL2CPP 兼容（无反射、无 `Activator.CreateInstance`、无 `MethodInfo.Invoke`）。

## 验收标准 (M4)

- [~] `Atlas.Shared.dll` 在 .NET 9 编译通过；Unity IL2CPP 侧验收工程尚未搭建。
- [x] `Atlas.Generators.Def` 生成属性/序列化/脏标记/工厂/Mailbox/Dispatcher/RPC ID/类型注册。
- [x] `Atlas.Generators.Events` 生成事件注册代码。
- [x] 生成代码中不出现 `System.Reflection` / `Activator.CreateInstance` / `MethodInfo.Invoke` / `PropertyInfo` / `GetType()`。

---

## `Atlas.Shared` — `src/csharp/Atlas.Shared/`

```
Atlas.Shared/
├── Atlas.Shared.csproj
├── DataTypes/
│   ├── Vector3.cs          — IEquatable<T> + operator ==/!= + GetHashCode()
│   ├── Quaternion.cs
│   └── EntityRef.cs
├── Entity/
│   ├── Attributes.cs       — 仅保留 [Entity(typeName, Compression = ...)]
│   ├── IEntityDef.cs
│   └── VolatileInfo.cs
├── Serialization/
│   ├── SpanWriter.cs       — ref struct, ArrayPool<byte>，小端
│   └── SpanReader.cs       — ref struct
├── Events/
│   ├── EventBus.cs         — 进程内同步分发
│   └── EventHandlerAttribute.cs
├── Rpc/
│   ├── IRpcProxy.cs
│   └── MailboxTarget.cs    — OwnerClient / AllClients / OtherClients / ...
└── Protocol/
    ├── IRpcTarget.cs
    └── MessageIds.cs
```

### 约束

- Target `netstandard2.1`，Unity 2021+ 兼容。
- **零外部依赖**：不引用 `Atlas.Runtime`（它依赖 `NativeApi` / `unsafe`）。
- `Atlas.Runtime` 引用 `Atlas.Shared`，反向引用禁止。
- `Vector3` / `Quaternion` 必须值相等（生成的脏标记 setter 依赖 `if (_field != value)`）。

## `Atlas.Generators.Def` — `src/csharp/Atlas.Generators.Def/`

唯一入口 `DefGenerator`（`IIncrementalGenerator`），读取 `entity_defs/*.def` XML 并交由 `Emitters/` 下的各发射器：

| Emitter | 产出 |
|---------|------|
| `PropertiesEmitter.cs` | 每个 `.def` 属性对应的 C# 属性 + 字段（`[Replicated]` 不再需要） |
| `SerializationEmitter.cs` | `Serialize(ref SpanWriter)` / `Deserialize(ref SpanReader)` |
| `DeltaSyncEmitter.cs` | 脏标记 `Flags` + `Serialize*Delta` / `Apply*Delta` / `ClearDirty` |
| `FactoryEmitter.cs` | `EntityFactory.Create(typeName)` —— `static () => new T()` 构造器表 |
| `MailboxEmitter.cs` | 对每个 `.def` 方向生成 `{Entity}ClientMailbox` / `{Entity}CellMailbox` / `{Entity}BaseMailbox` `readonly struct` |
| `RpcStubEmitter.cs` | Mailbox 方法体：序列化参数 → `SendClientRpc / SendCellRpc / SendBaseRpc` |
| `DispatcherEmitter.cs` | 接收端 `RpcDispatcher` 分发 switch，反序列化参数后调用 `OnXxx` 处理器 |
| `RpcIdEmitter.cs` | RPC ID 常量，格式 `0xTTTT_DDNN` —— `TypeIndex / Direction / MethodIndex` |
| `TypeRegistryEmitter.cs` | `DefEntityTypeRegistry` + `[ModuleInitializer]` → 通过 `NativeApi.RegisterEntityType` 注册到 C++ `EntityDefRegistry` |

辅助文件：`DefModel.cs`（解析后的模型）、`DefParser.cs`（XML 解析）、`DefTypeHelper.cs`、`DefDiagnosticDescriptors.cs`、`ProcessContext.cs`。

## `Atlas.Generators.Events` — `src/csharp/Atlas.Generators.Events/`

- 入口：`EventGenerator`（`IIncrementalGenerator`），扫描实现 `Atlas.Events.IEventListener` 且带 `[Atlas.Events.EventHandler("name")]` 方法的 partial class。
- 为每个 handler 方法生成内部 `IEventHandler` 实现类（因为 `SpanReader` 是 `ref struct`，不能作为 `Action<>` 参数）。
- 为宿主类生成 `Atlas_RegisterEventHandlers(EventBus)` / `Atlas_UnregisterEventHandlers(EventBus)`。
- 诊断：`EventDiagnosticDescriptors.cs`。

## 测试

| 项目 | 覆盖 |
|------|------|
| `tests/csharp/Atlas.Generators.Tests/DefGeneratorTests.cs` | DefGenerator 的属性/序列化/Mailbox/Dispatcher/RPC ID/诊断 |
| `tests/csharp/Atlas.Runtime.Tests/` | 由 `.def` 生成的代码在运行时正确（脏标记、工厂、往返序列化） |

## 剩余工作

- `Atlas.Generators.Events` 的快照 / 诊断专用测试。
- Unity IL2CPP 端验收工程（目标平台 iOS / Android），需要 `link.xml` 指引 + 端到端运行一次序列化/RPC/事件往返。
