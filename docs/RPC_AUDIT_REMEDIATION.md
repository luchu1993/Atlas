# Atlas RPC 实现状态、缺陷修复与后续任务

> 来源：2026-04-18 RPC 实现深度审计 + 从 `BIGWORLD_RPC_REFERENCE.md` 摘出的 Atlas 实现章节
> 关联：[BigWorld RPC 参考](BIGWORLD_RPC_REFERENCE.md)（纯 BigWorld 机制参考，不含 Atlas 实现细节） | [Phase 8 BaseApp](roadmap/phase08_baseapp.md) | [Phase 10 CellApp](roadmap/phase10_cellapp.md)
>
> 本文档覆盖 Atlas 侧与 RPC 相关的全部工程条目：
> 1. **缺陷修复清单**（§0 ~ §6）：已实现代码中的结构性缺陷（C-1 ~ L-4）
> 2. **Atlas ↔ BigWorld 映射**（§7）：概念对照、安全校验映射、关键实现建议
> 3. **Atlas RPC 实现状态**（§8）：六路径状态矩阵、相对 BigWorld 的架构优势
> 4. **Phase 10 / 11 任务清单**（§9 ~ §10）：CellApp 解阻、AOI 扇出、Real/Ghost（P0-1 ~ P1-5）
> 5. **安全加固路线图**（§11）：Atlas 独有的 hardening 机会（S-1 ~ S-4）
> 6. **任务依赖与验收里程碑**（§12）
> 7. **Atlas 设计注记**（§13）：从 BigWorld 参考文档散落段落汇总的实现对应说明
>
> 执行顺序：阻塞性修复（C-1 ~ C-4）应先于 Phase 10 任务（P0-1 ~ P0-5）落地。

---

## 0. 缺陷优先级一览

| 编号 | 严重度 | 类别 | 主题 | 预估工作量 | 目标里程碑 |
|---|---|---|---|---|---|
| C-1 | 🔴 Critical | 序列化 | int8/uint8 方法缺失导致生成代码无法编译 | 15 min | 立即 |
| C-2 | 🔴 Critical | RPC ID | 位段溢出无校验，静默冲突 | 30 min | 立即 |
| C-3 | 🔴 Critical | RPC ID | 方法索引按字母序漂移 + 无协议版本握手 | 1 ~ 2 天 | 立即 |
| C-4 | 🔴 Critical | 初始化 | ModuleInitializer 顺序不保证 | 2 h | 立即 |
| H-1 | 🟠 High | BaseApp | `OnCellRpcForward` 缺实体存活检查 | 30 min | Phase 10 前 |
| H-2 | 🟠 High | C# Dispatcher | switch 无 default，未知 RPC 静默丢失 | 20 min | Phase 10 前 |
| H-3 | 🟠 High | RPC ID | Entity Type ID 跨构建漂移 | 半天 | Phase 10 前 |
| M-1 | 🟡 Medium | 观测 | Dispatcher 反序列化错误无上下文 | 1 h | Phase 10 内 |
| M-2 | 🟡 Medium | 生命周期 | `IsDestroyed` 实体仍可被分发 | 30 min | Phase 10 内 |
| M-3 | 🟡 Medium | 重入 | RPC 递归深度无防护 | 1 h | Phase 10 内 |
| M-4 | 🟡 Medium | 协议 | Payload 最大尺寸未校验 | 1 h | Phase 10 内 |
| M-5 | 🟡 Medium | 序列化 | 有符号整数走 PackedUInt 浪费带宽 | 2 h | 选做 |
| M-6 | 🟡 Medium | 测试 | RPC 测试覆盖不足 | 1 ~ 2 天 | 随各修复落地 |
| L-1 | 🟢 Low | 观测 | 无 RPC 计数指标 | 半天 | Phase 11 |
| L-2 | 🟢 Low | 观测 | 无 trace ID 贯通 | 半天 | Phase 11 |
| L-3 | 🟢 Low | 协议 | 无重复消息去重 | — | 接受（TCP 场景不需要） |
| L-4 | 🟢 Low | 文档 | 初始化顺序文档缺失 | 随 C-4 一起 | 立即 |

总工作量估算：**Critical 合计 ~2-3 天 + High 合计 ~1 天 + Medium ~3-4 天**。

---

## 1. Critical 级缺陷（阻塞下一版提交）

### C-1 补齐 int8 / uint8 序列化方法

#### 现象
`DefTypeHelper.cs:35-60` 把 `int8` / `uint8` 类型映射到 `WriteInt8` / `WriteUInt8` / `ReadInt8` / `ReadUInt8` 方法调用，但 `SpanWriter.cs` / `SpanReader.cs` 中**这些方法全部不存在**。

任何在 `.def` 里写 `<arg type="int8"/>` 或 `<arg type="uint8"/>` 的方法，生成代码会立即编译失败。

#### 根因
DefTypeHelper 的映射表是按规划写的，但 Span 序列化器只加了 int16/int32/int64 等宽类型的实现，遗漏了单字节签名变体。

#### 修复步骤

**文件**：`src/csharp/Atlas.Shared/Serialization/SpanWriter.cs`

在 `WriteByte` 方法附近追加：
```csharp
public void WriteInt8(sbyte value) => WriteByte((byte)value);
public void WriteUInt8(byte value) => WriteByte(value);
```

**文件**：`src/csharp/Atlas.Shared/Serialization/SpanReader.cs`

在 `ReadByte` 方法附近追加：
```csharp
public sbyte ReadInt8() => (sbyte)ReadByte();
public byte ReadUInt8() => ReadByte();
```

#### 测试
`tests/csharp/Atlas.Shared.Tests/Serialization/SpanWriterReaderTests.cs` 新增：
```csharp
[Theory]
[InlineData((sbyte)0)]
[InlineData((sbyte)127)]
[InlineData((sbyte)-128)]
[InlineData((sbyte)-1)]
public void WriteReadInt8_RoundTrip(sbyte value) { /* writer + reader round-trip assertion */ }

[Theory]
[InlineData((byte)0)]
[InlineData((byte)255)]
public void WriteReadUInt8_RoundTrip(byte value) { /* round-trip assertion */ }
```

#### 验收
- 单元测试通过
- 临时创建一个使用 `int8` 参数的 `.def`，DefGenerator 产物能编译并 round-trip

---

### C-2 RpcIdEmitter 加位段边界校验

#### 现象
`RpcIdEmitter.cs:46` 的 RPC ID 合成代码：
```csharp
int id = (direction << 22) | (typeIndex << 8) | (i + 1);
```
未校验位段范围。当：
- `typeIndex > 0x3FFF`（超过 16383 个实体类型）→ 溢出进入 direction 位
- `i + 1 > 0xFF`（单实体方法数超过 255）→ 溢出进入 typeIndex 位

都会产生**静默的 ID 冲突**，两个不同方法拿到同一 ID，客户端调 A 可能路由到 B。

#### 根因
位域编码未加范围 assert，依赖开发者"不会写那么多"的默契。

#### 修复步骤

**文件**：`src/csharp/Atlas.Generators.Def/Emitters/RpcIdEmitter.cs`

在 `EmitMethodIds` 方法内加校验：
```csharp
private static void EmitMethodIds(StringBuilder sb, string entityName,
    List<MethodDefModel> methods, ushort typeIndex, byte direction, string comment)
{
    if (methods.Count == 0) return;

    if (typeIndex > 0x3FFF)
        throw new InvalidOperationException(
            $"Entity type '{entityName}' typeIndex={typeIndex} exceeds 14-bit limit (0x3FFF). " +
            "Atlas RPC ID format supports at most 16383 entity types.");

    if (methods.Count > 0xFF)
        throw new InvalidOperationException(
            $"Entity type '{entityName}' has {methods.Count} {comment} methods, exceeds 8-bit limit (255). " +
            "Atlas RPC ID format supports at most 255 methods per entity per direction.");

    sb.AppendLine($"    // {entityName} {comment}");
    // ... 原有逻辑
}
```

同样在 `DispatcherEmitter.cs` / `MailboxEmitter.cs` 等任何按相同位域合成 ID 的地方加同样 assert（grep `(direction << 22)` 和 `(typeIndex << 8)` 找到所有点）。

**对称地在 C++ 侧** `src/lib/entitydef/entity_type_descriptor.h` 的 `RpcDescriptor` 构造或注册路径加：
```cpp
static_assert(/* ... */);
// 或运行时：
if (type_index > 0x3FFF) ATLAS_FATAL("type_index overflow");
```

#### 测试
`tests/csharp/Atlas.Generators.Def.Tests/RpcIdEmitterTests.cs` 新增：
```csharp
[Fact]
public void EmitMethodIds_TypeIndexOverflow_Throws()
{
    var methods = new List<MethodDefModel> { new() { Name = "m" } };
    var sb = new StringBuilder();
    Assert.Throws<InvalidOperationException>(() =>
        RpcIdEmitter.Emit(/* typeIndex = 0x4000 */));
}

[Fact]
public void EmitMethodIds_TooManyMethods_Throws()
{
    var methods = Enumerable.Range(0, 256).Select(i => new MethodDefModel { Name = $"m{i:D3}" }).ToList();
    // 预期抛 InvalidOperationException
}
```

#### 验收
- 新单元测试通过
- 正常规模（~100 实体 × ~20 方法）生成无变化

---

### C-3 方法索引稳定化 + Entity Def Digest 握手

#### 现象
`RpcIdEmitter.cs:43` 与 `DispatcherEmitter.cs` 把方法按 `OrderBy(m => m.Name)` 字母序排，`method_index = 字母序位次 + 1`。

**问题场景**：
- `.def` 里新增一个字母序靠前的方法（例：原有 `Attack` → 新增 `Ability`）
- 所有字母序靠后的方法 ID 都会**右移 1 位**
- 若只重编了服务端，客户端用旧 ID 调 `Attack`，服务端路由到新 `Ability` → 静默调错
- 无任何 checksum 兜底，运行时完全沉默

#### 根因
1. 方法索引依赖名称字母序（而非声明顺序），任何重命名、新增、删除都可能引起漂移
2. 客户端/服务端无协议版本握手，不一致时不中断连接而是继续运行

#### 修复步骤

**Part A — 索引按 `.def` 声明顺序分配（推荐方案）**

**文件**：`src/csharp/Atlas.Generators.Def/Emitters/RpcIdEmitter.cs`
```csharp
// 删除这行：
// var sorted = methods.OrderBy(m => m.Name).ToList();
// 改为：
var ordered = methods;  // 直接用 DefParser 的声明顺序
```

**文件**：`src/csharp/Atlas.Generators.Def/Emitters/DispatcherEmitter.cs`
对应位置删掉 `OrderBy(m => m.Name)`，改为沿用 `methods` 的原始顺序。

**文件**：`src/csharp/Atlas.Generators.Def/Emitters/MailboxEmitter.cs`
同上。

**文件**：`src/csharp/Atlas.Generators.Def/DefParser.cs`
确认 `ClientMethods` / `CellMethods` / `BaseMethods` 按 `XElement.Elements("method")` 顺序填充（已经是，无需改）。

**文件**：`src/csharp/Atlas.Generators.Def/DefModel.cs`
`MethodDefModel` 新增 `DeclarationIndex` 字段（0-based），由 DefParser 按读取顺序填入，供下游 emitter 使用。这样即使中间有 reorder 工具也能保持稳定。

**Part B — 增加 `.def` 显式 `index` 属性（可选，用于超大项目）**

允许在 `.def` 中显式指定：
```xml
<method name="attack" index="5">
  <arg type="int32"/>
</method>
```
- 若 `index` 提供 → 直接用，DefParser 校验不重复
- 若未提供 → 自动按声明顺序递增分配（从当前已占用的最大值 + 1 起）
- 删除方法需保留占位 `<method name="removed_x" index="3" deprecated="true"/>` 防止 index 回收

**Part C — EntityDef Digest 启动握手**

**目标**：客户端/服务端用不同 .def 构建时，连接建立阶段立刻失败，而非运行时静默错乱。

**C.1 生成 digest**

`src/csharp/Atlas.Generators.Def/DefGenerator.cs` 新增 emitter：
```csharp
// 产物：Atlas.Rpc.EntityDefDigest.g.cs
public static class EntityDefDigest
{
    public const string Sha256 = "a1b2c3..."; // 所有 entity 的 name + method (name,direction,args) 规范化后 SHA-256
}
```

Digest 输入规范化字符串：
```
Entity:Avatar\n
  Client:showEffect:uint32\n
  Cell:useItem[own_client]:uint32\n
  Base:logOff[own_client]:\n
Entity:Npc\n
  ...
```
- Entity 按 name 排序（不影响 digest 内容，保证稳定）
- 每 entity 内方向固定顺序（Client / Cell / Base）
- 每方向内方法按 **声明顺序**（与运行时 RPC ID 分配一致）
- 参数按声明顺序，仅类型名

**C.2 C++ 侧对称生成**

`src/lib/entitydef/entity_def_registry.cc` 提供 `ComputeDigest()` 返回同一字符串的 SHA-256。首次 `RegisterType` 调用链结束后缓存为 `entity_def_digest_`。

**C.3 握手协议**

`src/lib/connection/login_messages.h` 的 `LoginRequest` 增加字段：
```cpp
struct LoginRequest {
  // 现有字段 ...
  std::array<uint8_t, 32> entity_def_digest{};  // SHA-256
};
```

`src/server/loginapp/loginapp.cc` 的 `OnLogin` 在密码校验前先比对：
```cpp
if (req.entity_def_digest != EntityDefRegistry::Instance().digest()) {
  SendLoginFailure(ch, LoginError::kDefMismatch, "Client/server entity definitions mismatch");
  return;
}
```

客户端 SDK 在 `Connect()` 时自动带上 `EntityDefDigest.Sha256`。

#### 测试
- 单元测试：修改 .def 里一个方法名 → digest 必须变化
- 集成测试：构造 digest 不一致的 client/server → 连接立刻被拒并返回清晰错误
- 回归测试：无变化的 .def 在两次独立构建中 digest 稳定

#### 验收
- .def 中间插入新方法不影响既有方法 RPC ID（可通过比较 `RpcIds.g.cs` 前后 diff 证明）
- digest 握手失败时 LoginApp 返回 `kDefMismatch` 错误码
- 文档更新 BIGWORLD_RPC_REFERENCE.md 的方法索引章节，说明 Atlas 侧按声明顺序分配的 index 规则

---

### C-4 ModuleInitializer 顺序明确化

#### 现象
`TypeRegistryEmitter.cs:34` 和 `DispatcherEmitter.cs:51` 分别为 `DefEntityTypeRegistry.RegisterAll` 和 `DefRpcDispatcher.RegisterDefDispatchers` 打了 `[ModuleInitializer]` 属性。

.NET 的 `[ModuleInitializer]` 只保证在"模块首次使用前"执行，不保证**多个 ModuleInitializer 之间的相对顺序**。`NativeCallbacks.Register()` 也是 ModuleInitializer。

若调度顺序是 `Dispatcher → TypeRegistry`，EntityDefRegistry 尚未填充 → C++ `FindRpc` 全部返回 null → 首波 RPC 全部被拒。

若顺序是 `TypeRegistry → Dispatcher`，并发场景下首个 RPC 到达时可能命中 `RpcBridge.Dispatchers[dir] == null` → RPC 被静默丢（`dispatcher?.Invoke` 是 null-coalesce）。

#### 根因
依赖隐式初始化顺序，运行时行为取决于程序集加载次序，跨环境不稳定。

#### 修复步骤

**Part A — 统一入口，移除分散的 ModuleInitializer**

**文件**：`src/csharp/Atlas.Runtime/Core/EngineContext.cs`

当前靠 ModuleInitializer 隐式执行。改为：
```csharp
public static class EngineContext
{
    private static int _initialized;

    public static void Initialize()
    {
        if (Interlocked.Exchange(ref _initialized, 1) == 1) return;

        // 1. C++ callback 注册必须最先（否则后续调用 NativeApi 会报错）
        NativeCallbacks.Register();

        // 2. 填充 EntityDefRegistry（C++ 侧拿到所有类型元数据）
        DefEntityTypeRegistry.RegisterAll();

        // 3. 注册 RPC dispatcher（此后 C++ 收到 RPC 可正确分发）
        DefRpcDispatcher.RegisterDefDispatchers();

        // 4. 其他子系统初始化（属性 Accessor 注册等）
        // ...

        Atlas.Log.Info("EngineContext initialized");
    }
}
```

**Part B — 移除生成代码中的 `[ModuleInitializer]`**

**文件**：`src/csharp/Atlas.Generators.Def/Emitters/TypeRegistryEmitter.cs:34`

删除 `sb.AppendLine("    [System.Runtime.CompilerServices.ModuleInitializer]");` 这一行，方法保持 `internal static`，由 `EngineContext.Initialize()` 显式调用。

**文件**：`src/csharp/Atlas.Generators.Def/Emitters/DispatcherEmitter.cs:51`

同样删除 `[ModuleInitializer]`。

**文件**：`src/csharp/Atlas.Runtime/Core/NativeCallbacks.cs`

如果 `Register()` 当前也是 ModuleInitializer，改为显式调用。

**Part C — 添加启动校验**

`EntityDefRegistry` 首次收到 RPC 时若 `_initialized == 0` → 抛 `InvalidOperationException("EngineContext.Initialize() not called")`，在开发期立刻暴露问题。

**Part D — CLR Host 调用 `EngineContext.Initialize`**

`src/lib/script/clr_host.cc` 加载 CLR 并拿到 managed entry point 后，第一步调用 `EngineContext::Initialize`（已在设计中，此处只是验证链路通）。

#### 测试
- 单元测试：手动构造"未调用 Initialize 先 dispatch"场景 → 断言抛出特定异常
- 集成测试：BaseApp 启动日志应包含 "EngineContext initialized" 后才有任何 RPC 日志
- 并发测试：多线程同时调 `Initialize` 只应实际初始化一次（`Interlocked.Exchange` 保障）

#### 验收
- 删除 `[ModuleInitializer]` 属性后，所有启动日志顺序固定
- 新增 README 段落记录初始化顺序契约（见 L-4）

---

## 2. High 级缺陷（Phase 10 前并入）

### H-1 `OnCellRpcForward` 补实体存活检查

#### 现象
`src/server/baseapp/baseapp.cc:595-603` 附近的 `OnCellRpcForward` 不同于 `OnSelfRpcFromCell` / `OnBroadcastRpcFromCell`：它不调用 `entity_mgr_.FindProxy(...)` 就直接 dispatch 到 C#。

虽然 C# 侧 `NativeCallbacks.DispatchRpc` 有 `entity is null` 兜底（`NativeCallbacks.cs:165`），但 C++ 侧无 warn 日志 → 运维调试困难。

#### 修复步骤

**文件**：`src/server/baseapp/baseapp.cc` `OnCellRpcForward` 方法

参照 `OnSelfRpcFromCell` 的写法，加前置校验：
```cpp
void BaseApp::OnCellRpcForward(Channel& ch, const CellRpcForward& msg) {
  auto* base = entity_mgr_.FindBase(msg.base_entity_id);
  if (!base) {
    ATLAS_LOG_WARN("CellRpcForward for unknown base entity {} (rpc_id=0x{:X})",
                   msg.base_entity_id, msg.rpc_id);
    return;
  }
  // ... 原有 dispatch 逻辑
}
```

#### 测试
集成测试：CellApp 向已销毁 Base 发 `CellRpcForward` → BaseApp 日志包含 WARN，无崩溃，C# 不被调用。

#### 验收
- 测试通过
- 所有 BaseApp Cell→Base / Cell→Client 方向的 handler 都有对称的存在性检查

---

### H-2 Dispatcher switch 加 default 分支

#### 现象
`DispatcherEmitter.cs` 生成的 `Dispatch_{EntityName}_*Rpc` 方法是：
```csharp
switch (rpcId) {
    case 0x08000501: /* method A */ break;
    case 0x08000502: /* method B */ break;
}
```
未知 RPC ID 直接 fall-through，无日志无异常。若客户端版本 / 服务端版本方法集合不一致（C-3 修复前常见），表现为 "调用没反应"，极难排查。

#### 修复步骤

**文件**：`src/csharp/Atlas.Generators.Def/Emitters/DispatcherEmitter.cs`

在生成 switch 结尾添加 default 分支：
```csharp
sb.AppendLine("            default:");
sb.AppendLine($"                Atlas.Log.Warning(\"Unknown {direction} RPC ID 0x{{0:X6}} for entity type {entityName}\", rpcId);");
sb.AppendLine("                break;");
```

（`direction` 为 "ClientRpc" / "CellRpc" / "BaseRpc"，便于日志定位方向）

#### 测试
单元测试：给 dispatcher 传一个不存在的 rpc_id → 断言 `Atlas.Log` 收到 Warning（用 log sink 替身）。

#### 验收
- 生成代码包含 default 分支
- 未知 RPC ID 产生可 grep 的 warning 日志

---

### H-3 Entity Type ID 跨构建稳定化

#### 现象
`DefGenerator.cs:92-95` 按字母序给实体分配 type_id（1-base）。若构建 A 包含 `[Boss, Npc, Player]`，`Npc.type_id = 2`；构建 B 去掉 Boss，`Npc.type_id = 1`。

**踩坑场景**：
- DBApp 用 type_id 做持久化 key → 不同构建读同一数据库会错位
- 客户端/服务端分别打包不同子集 → type_id 不匹配

C-3 的 digest 握手能检测到错位，但更好的做法是**从源头保证 type_id 稳定**。

#### 修复步骤

**Part A — .def 声明 `id` 属性**

**文件**：`src/csharp/Atlas.Generators.Def/DefParser.cs`

已经支持 `<entity name="Avatar" id="1">`（见本文档 §7.1 Atlas 概念对照表）。确认 DefParser 把 `id` 作为 `EntityDefModel.TypeId` 字段。

**文件**：`src/csharp/Atlas.Generators.Def/DefGenerator.cs`

修改 type_id 分配逻辑：
```csharp
// 原：按字母序 1-base 自动分配
// 新：
foreach (var def in defs) {
    if (def.TypeId <= 0)
        throw new InvalidOperationException(
            $"Entity '{def.Name}' missing required 'id' attribute in .def file");
    if (typeIndexMap.ContainsValue((ushort)def.TypeId))
        throw new InvalidOperationException(
            $"Entity '{def.Name}' type_id={def.TypeId} duplicated");
    typeIndexMap[def.Name] = (ushort)def.TypeId;
}
```

**Part B — 落地一个 `entity_ids.xml` 清单**

项目根目录 `game/entities/entity_ids.xml`（或等价位置）集中管理：
```xml
<entity_ids>
  <entity name="Avatar" id="1"/>
  <entity name="Npc"    id="2"/>
  <entity name="Boss"   id="3"/>
  <!-- 删除实体时保留 <entity name="_removed_boss" id="3" deprecated="true"/> 防止 id 复用 -->
</entity_ids>
```

DefParser 加载所有 `.def` 时先读此清单校验：
- 每个实体必须在清单里有 id
- 删除实体不得释放 id（只标 deprecated）
- CI 校验清单单调递增、无回收

**Part C — CI 兜底**

在 CI 脚本加一步：
```bash
atlas_rpc_audit --check-stable-ids game/entities/*.def game/entities/entity_ids.xml
```
该工具（未来 `tools/rpc_id_audit/`）对比当前构建产物与上次提交的 `RpcIds.g.cs`，若同名方法 / 同名实体的 ID 发生变化则失败。

#### 测试
- 缺 id 的 .def → DefGenerator 构建失败
- id 重复 → 构建失败
- 两次构建，第二次删除一个中间 entity 但清单保留 deprecated → 剩余实体 id 不变

#### 验收
- 所有现有 .def 都加了 id 属性
- `entity_ids.xml` 清单文件已创建
- CI 检查通过

---

## 3. Medium 级缺陷（Phase 10 内清理）

### M-1 Dispatcher 反序列化错误附带上下文

#### 现象
生成的 dispatcher 里 `reader.ReadXxx()` 若抛 `InvalidOperationException`（payload 截断），异常消息只有 "Insufficient data"，无法定位是哪个实体、哪个方法、哪个参数。

#### 修复步骤

**文件**：`src/csharp/Atlas.Generators.Def/Emitters/DispatcherEmitter.cs`

把 case 分支包进 try-catch：
```csharp
case 0x{rpcId:X6}:
{
    try
    {
        var arg1 = reader.ReadInt32();
        var arg2 = reader.ReadString();
        target.{methodName}(arg1, arg2);
    }
    catch (Exception ex)
    {
        throw new RpcDispatchException(
            $"Failed to dispatch {entityName}.{methodName} (rpc_id=0x{rpcId:X6})", ex);
    }
    break;
}
```

新增 `Atlas.Runtime/Rpc/RpcDispatchException.cs`：
```csharp
public class RpcDispatchException : Exception
{
    public RpcDispatchException(string message, Exception inner) : base(message, inner) { }
}
```

`NativeCallbacks.DispatchRpc` 的 catch 分支捕获 `RpcDispatchException` 单独处理，记录 entity_id + rpc_id + 栈。

#### 验收
畸形 payload 触发时日志包含：实体名、方法名、rpc_id、原始异常消息。

---

### M-2 Dispatcher 检查 `entity.IsDestroyed`

#### 现象
`NativeCallbacks.DispatchRpc` 有 `entity is null` 判断（`NativeCallbacks.cs:165`），但 `ServerEntity.IsDestroyed`（`ServerEntity.cs:16`）为 true 的已销毁实体**引用仍在**，会被当作有效实体 dispatch，脚本层可能操作到已被清理的状态。

#### 修复步骤

**文件**：`src/csharp/Atlas.Runtime/Interop/NativeCallbacks.cs`

在 `entity is null` 之后增加：
```csharp
if (entity is null) return;
if (entity.IsDestroyed)
{
    Atlas.Log.Debug("Dropping RPC for destroyed entity {EntityId}", entityId);
    return;
}
```

#### 测试
构造场景：entity 标记 `IsDestroyed = true` 后投递一条 RPC → 断言 `Method()` 未被调用、log debug 被记录。

---

### M-3 RPC 递归深度保护

#### 现象
C# 方法 A 调 `self.client.B()` → Mailbox 立即触发 dispatch（如果目标是本地 entity，例如测试场景的 loopback） → 重新进入 A → 栈溢出。

虽然当前实际 RPC 都走网络（不立即 dispatch），但框架允许自定义 dispatch 路径（如单元测试 in-process loopback），隐患存在。

#### 修复步骤

**文件**：`src/csharp/Atlas.Runtime/Core/ThreadGuard.cs`（若无此文件则新建）

```csharp
public static class ThreadGuard
{
    [ThreadStatic] private static int _rpcDepth;
    private const int kMaxRpcDepth = 32;

    public readonly struct RpcScope : IDisposable
    {
        public RpcScope()
        {
            if (_rpcDepth >= kMaxRpcDepth)
                throw new InvalidOperationException(
                    $"RPC dispatch depth exceeded {kMaxRpcDepth}, possible recursion");
            _rpcDepth++;
        }
        public void Dispose() => _rpcDepth--;
    }
}
```

**文件**：`src/csharp/Atlas.Runtime/Interop/NativeCallbacks.cs`

`DispatchRpc` 的方法体用 `using var _ = new ThreadGuard.RpcScope();` 包裹。

#### 验收
- 人工构造 32 层递归触发异常
- 正常单层 RPC 无可观测性能损失

---

### M-4 RPC Payload 最大尺寸硬限制

#### 现象
`ClientBaseRpc` / `ClientCellRpc` 等消息的 `payload` 字段最大长度仅受 Channel 帧长限制（MB 级），无 RPC 层显式限制。恶意客户端可构造超大参数（如 10MB string）触发 C# 大对象堆分配、GC 抖动。

#### 修复步骤

**文件**：`src/lib/entitydef/entity_def_config.h`（新建或在现有 config 处）

```cpp
namespace atlas {
inline constexpr size_t kMaxRpcPayloadBytes = 64 * 1024; // 64 KB
}
```

**文件**：`src/server/baseapp/baseapp.cc:OnClientBaseRpc` 与未来的 `OnClientCellRpc`

在 `FindRpc` 之后加：
```cpp
if (msg.payload.size() > kMaxRpcPayloadBytes) {
  ATLAS_LOG_WARN("RPC payload {} bytes exceeds max {} from channel {}",
                 msg.payload.size(), kMaxRpcPayloadBytes, ch.remote_address());
  // TODO(S-1): 此类违规记入 rate limiter，超阈值断连
  return;
}
```

按方法级可覆盖（.def 里的 `<method max_payload="128K"/>`）作为后续增强。

#### 验收
客户端发 65 KB payload → BaseApp 丢弃并记 warn，连接保持；合法请求不受影响。

---

### M-5 有符号整数 ZigZag 编码（选做）

#### 现象
`SpanWriter.WritePackedUInt32` 对有符号负数（如 int32 = -1）直接 reinterpret cast 为 `uint32_t` = `0xFFFFFFFF` → 序列化成 4 字节。常见的小负数场景（相对坐标、差值）带宽浪费严重。

#### 修复步骤
仅在确定有收益的场景按需实施。流程：
1. 在 `SpanWriter.cs` / `SpanReader.cs` / 对应 C++ 添加 `WritePackedVarInt` / `ReadPackedVarInt`（ZigZag + varint）
2. `DefTypeHelper.cs` 为 `int32` / `int64` 映射到 `WritePackedVarInt`
3. **破坏性变更** — 需要与 C-3 的 digest 绑定，旧版本二进制不兼容

**建议**：暂不实施，改为按需在 `.def` 用 `<arg type="varint32"/>` 显式声明。

---

### M-6 RPC 测试套件补齐

#### 现象
`tests/` 下 RPC 相关测试仅覆盖 PendingRpcRegistry（coro RPC request/response 相关性），缺：
- 所有 DefTypeHelper 支持类型的 round-trip
- RPC ID 边界（最大 typeIndex / method count）
- Digest 一致性与错位检测
- Dispatcher 未知 ID 日志
- 初始化顺序违规检测
- Payload 超限检测

#### 修复步骤

新增测试文件：
- `tests/csharp/Atlas.Shared.Tests/Serialization/SpanRpcTypeCoverageTests.cs`
  - `[Theory]` 遍历 DefTypeHelper 支持的每个类型
- `tests/csharp/Atlas.Generators.Def.Tests/RpcIdBoundsTests.cs`
  - typeIndex 0 / 1 / 0x3FFF / 0x4000（最后一项应 throw）
  - method count 255 / 256
- `tests/csharp/Atlas.Generators.Def.Tests/EntityDefDigestStabilityTests.cs`
  - 同一输入 .def 两次生成 digest 相同
  - 任一 entity / method / arg type 变化 → digest 变化
- `tests/integration/test_rpc_version_handshake.cc`
  - 两个 BaseApp 用不同 digest → 登录被拒
- `tests/unit/test_baseapp_rpc_payload_limit.cc`
  - 超长 payload → 被丢弃 + warn

#### 验收
CI 跑通全部新增测试；覆盖率报告中 `RpcStubEmitter` / `DispatcherEmitter` / `OnClientBaseRpc` 覆盖率 ≥ 80%。

---

## 4. Low 级（后续加固）

### L-1 RPC 计数指标
在 `EntityApp` 引入 metrics registry（prometheus-style counter）：
- `atlas_rpc_received_total{direction, entity_type, method}`
- `atlas_rpc_dispatch_failed_total{reason}`
- `atlas_rpc_exposed_violation_total`

通过 BaseAppMgr 的 status endpoint 暴露给运维面板。

### L-2 Trace ID 贯通
RPC 消息增加可选 `uint64 trace_id`（0 表示无）。BaseApp → CellApp → Client 转发时保留原 ID。C# 层入口记录 trace_id 到 `AsyncLocal<long>` 供脚本 Log 调用。

### L-3 重复消息去重
TCP 场景不需要，但未来若支持 UDP Reliable 路径，需在消息层加 seq + 滑动窗口。当前不做。

### L-4 初始化顺序文档
创建 `docs/SCRIPT_INITIALIZATION_ORDER.md` 记录：
1. `NativeCallbacks.Register()` — P/Invoke 契约建立
2. `DefEntityTypeRegistry.RegisterAll()` — C++ 侧实体元数据
3. `DefRpcDispatcher.RegisterDefDispatchers()` — RPC 路由表
4. `EntityFactory` 注册（DefGenerator 生成）
5. 用户脚本 `OnStartup()`

---

## 5. 修复顺序与提交建议

### 阶段 1：Critical 修复（合计 ~2-3 天，必须先合入）

建议顺序：
1. **C-1**（15 min）→ 独立 PR：`fix(rpc): add int8/uint8 serialization methods to SpanWriter/Reader`
2. **C-2**（30 min）→ 独立 PR：`feat(gen): validate RPC ID bit-field bounds in RpcIdEmitter`
3. **C-4**（2 h）→ 独立 PR：`refactor(runtime): explicit EngineContext.Initialize order`
4. **C-3**（1-2 天）→ 分两个 PR：
   - 4a: `refactor(gen): allocate method index by declaration order`（Part A+B）
   - 4b: `feat(net): add EntityDef digest handshake on login`（Part C）

### 阶段 2：High 修复（~1 天，与 Phase 10 前置任务同步推进）

5. **H-1**（30 min）→ `fix(baseapp): validate entity existence in OnCellRpcForward`
6. **H-2**（20 min）→ `feat(gen): log unknown RPC IDs in dispatcher default case`
7. **H-3**（半天）→ `feat(gen): require explicit entity id in .def files`

### 阶段 3：Medium 修复（Phase 10 内）

8. M-1 / M-2 / M-3 / M-4 随 Phase 10 的 ClientCellRpc 实施一起合入
9. M-5 先跳过，等实际性能瓶颈再做
10. M-6 与每个修复同步提交测试（而非最后集中补）

### 阶段 4：Low 加固（Phase 11 及以后）

11. L-1 / L-2 / L-4 视排期灵活安排

---

## 6. 快速验收检查单

每项修复合入前，必须通过以下自检：

- [ ] 改动文件只限修复范围，无附带重构
- [ ] 单元测试覆盖正常路径 + 至少一条边界 / 错误路径
- [ ] `ctest --build-config Debug --label-regex unit` 全绿
- [ ] `clang-format --dry-run --Werror` 无违规
- [ ] C# 侧 `dotnet build -warnaserror` 无警告
- [ ] 变更点已在本文档对应章节勾选 "已完成" 或在 commit message 引用对应编号（例：`Fixes RPC-AUDIT C-1`）

---

## 7. Atlas ↔ BigWorld 概念与安全映射

> 本节摘自原 `BIGWORLD_RPC_REFERENCE.md §10`，为 Atlas 侧实现提供对 BigWorld 设计点的对照。

### 7.1 概念对照

| BigWorld 概念 | Atlas 对应 | 状态 | 说明 |
|---|---|---|---|
| `.def` XML 方法声明 | `.def` XML（`<client_methods>`/`<cell_methods>`/`<base_methods>`）| ✅ | `DefParser.cs:54-97`；DefGenerator 按进程上下文生成代码 |
| `<Exposed/>` 标记 | `.def` 的 `<exposed>` 属性（`none`/`own_client`/`all_clients`） | ✅ | `DefParser.cs:134-145`；与方向正交 |
| `PyServer` + `ServerCaller`（客户端调服务端） | `{Entity}CellMailbox` / `{Entity}BaseMailbox` readonly struct | ✅ 生成 / ⚠️ 网络路径见 §8.1 | `MailboxEmitter.cs:7-142` |
| `PyClient` + `ClientCaller`（Cell 调客户端） | `{Entity}ClientMailbox` readonly struct + `MailboxTarget` | ⚠️ 部分 | MailboxTarget 枚举已定义但 Cell 侧三个属性（`ownClient`/`otherClients`/`allClients`）尚未生成；无 Witness 扇出 |
| `CellEntityMailBox`（Base→Cell）| `{Entity}CellMailbox` | ⚠️ 部分 | Mailbox 代码已生成，但服务器间通路（`InternalCellRpc`）尚未实现 |
| `ClientEntityMailBox`（Base→Client）| `{Entity}ClientMailbox` on Proxy | ✅ | Base→Client 直发已通 |
| `RemoteEntityMethod::pyCall()` | Mailbox 方法体（`SpanWriter` 栈上序列化） | ✅ | 零堆分配 |
| `MethodDescription::callMethod()` | `DefRpcDispatcher.Dispatch_*()` switch 分发 | ✅ | `DispatcherEmitter.cs:77-128` 编译期生成 |
| `ExposedMethodMessageRange` | `RpcIds` 常量（`0xTTTT_DDNN`） | ✅ | 方向 2b + 类型 14b + 方法 8b，无运行时范围编码 |
| `exposedMethod()` / `internalMethod()` 双索引 | 统一 RPC ID | ✅ | 方向位内嵌即区分路径 |
| `sourceEntityID` 验证 | `ClientCellRpc.caller_entity_id` + CellApp OWN_CLIENT 校验 | ❌ | **计划方案**；消息结构体未定义、CellApp 未实现（§9） |

### 7.2 安全校验映射

| BigWorld 检查 | Atlas 对应 | 实现位置 | 状态 |
|---|---|---|---|
| `isExposed()` 门禁 | `EntityDefRegistry::IsExposed(rpc_id)` | `src/lib/entitydef/entity_def_registry.h:46` → `baseapp.cc:2136` | ✅ 已实现 |
| `CLIENT_UNSAFE` 类型警告 | 编译期类型安全 (C# 强类型) | `Atlas.Generators.Def` | ✅ 已实现 |
| `CLIENT_UNUSABLE` 拒绝 | 不适用 (C# 无 MAILBOX/PYTHON 类型) | — | ✅ 结构性解决 |
| `REAL_ONLY` 消息路由 | CellApp 消息 handler 的 reality 检查 | Phase 11 | ❌ 未实现 |
| `sourceEntityID` 验证 | `ClientCellRpc.caller_entity_id` by BaseApp + CellApp OWN_CLIENT 校验 | Phase 10 | ❌ 未实现（§9 P0-2/P0-4） |
| 客户端 `ClientBaseRpc` 身份 | Channel → EntityID 索引认证 | `baseapp.cc:2122-2148` | ✅ 已实现 |
| 无 rate limiting | token bucket interceptor | BaseApp 外部接口 | ❌ 未实现（§11 S-1） |

### 7.3 关键实现建议

1. **客户端 RPC 校验必须在 C++ 层:**
   客户端消息先经过 `EntityDefRegistry::ValidateRpc(type_id, rpc_id)` +
   `IsExposed(rpc_id)` + `GetExposedScope(rpc_id)` 校验，确认:
   - rpc_id 对应的方法存在（等价于 BigWorld 加载时索引查找）
   - 方法标记了 exposed（等价于 `isExposed()` 检查）
   - `OWN_CLIENT` 方法的 target_entity_id == proxy.entity_id（等价于 sourceID 检查）
   通过后才转发给 C# `DefRpcDispatcher`。

2. **CellApp 的 caller_entity_id 由 BaseApp 填写:**
   与 BigWorld 的 `Proxy::cellEntityMethod()` 中 `b << id_` 完全一致。
   BaseApp 在 `OnClientCellRpc` → `ClientCellRpcForward` 转发时追加此字段，客户端无法伪造。

3. **MailboxTarget 三模式对齐 BigWorld:**
   BigWorld 的 `PyClient(isForOwn, isForOthers)` 对应 Atlas 的 `MailboxTarget` 枚举:
   - `OwnerClient` = `isForOwn=true, isForOthers=false`
   - `OtherClients` = `isForOwn=false, isForOthers=true`
   - `AllClients` = `isForOwn=true, isForOthers=true`

4. **AOI 下行路径当前约束:**
   CellApp 的 ClientRpc 需经 BaseApp Proxy 转发到客户端。已有消息：
   - `SelfRpcFromCell` (msg 2014) — Cell → 单一 Owner Client（经其 BaseApp）
   - `BroadcastRpcFromCell` (msg 2016) — 当前仅单目标发送，**未实现真正 AOI 扇出**
   Phase 10 需实现 Witness 在 CellApp 端按 AOI 展开观察者，再由 BaseApp 分发。

---

## 8. Atlas RPC 实现状态

> 本节摘自原 `BIGWORLD_RPC_REFERENCE.md §13.1 ~ §13.2`。

### 8.1 六条 RPC 路径实现状态矩阵

| 路径 | 消息 ID | C++ 消息结构 | C++ Handler | C# Send Stub | C# Receive Dispatcher | 端到端可用 |
|---|---|---|---|---|---|---|
| **Client → Base** | `ClientBaseRpc` = 2022 | ✅ `baseapp_messages.h:479-514` | ✅ `baseapp.cc:2122-2148` | ✅ `RpcStubEmitter` | ✅ `DefRpcDispatcher.Dispatch_*_BaseRpc` | ✅ |
| **Client → Cell** | `ClientCellRpc` = 2023 | ❌ ID 占位、结构体缺失 | ❌ 无 `OnClientCellRpc` | ⚠️ C# 侧已生成，发送时会失败 | ❌ CellApp 未实现 | ❌ |
| **Base → Cell** | `InternalCellRpc` = 3004（规划）| ❌ `cellapp_messages.hpp` 未创建 | ❌ | ✅ C# `CellMailbox` stub 已生成 | ❌ | ❌ |
| **Cell → Base** | `CellRpcForward` = 2013 | ✅ 已定义 | ✅ `OnCellRpcForward` | ❌ CellApp 不存在故无发送方 | ✅ C# Dispatcher | ⚠️ 半通 |
| **Base → Client** | Proxy 直发 | ✅ | ✅ | ✅ | ✅ | ✅ |
| **Cell → Client** OwnerClient | `SelfRpcFromCell` = 2014 | ✅ | ✅ | ❌ 无 Cell 发送方 | ✅ | ⚠️ 半通 |
| **Cell → Client** AllClients / OtherClients | `BroadcastRpcFromCell` = 2016 | ⚠️ 仅单目标 | ⚠️ 单目标实现 | ❌ 无 Cell 发送方 | ✅ | ❌ 无 AOI 扇出 |

图例：✅ 已实现 / ⚠️ 部分 / ❌ 未实现

### 8.2 Atlas RPC 相对 BigWorld 的架构优势

| 维度 | BigWorld | Atlas | 优势来源 |
|---|---|---|---|
| 方法索引 | 运行时双索引（exposed + internal） | 编译期统一 RPC ID `0xTTTT_DDNN` | 方向位内嵌，无需维护两套映射；客户端/服务端共享常量 |
| 方法定义解析 | 运行时 XML 解析 + `MethodDescription` 构造 | 编译期 Roslyn IncrementalGenerator | 启动零开销；类型/方向不一致时构建即失败 |
| Mailbox 调用 | Python 对象 + PyObject_CallObject | C# `readonly struct` + `SpanWriter` | 零堆分配；编译期内联 |
| 参数类型安全 | 运行时 `CLIENT_SAFE/UNSAFE/UNUSABLE` 三级 | C# 强类型 + DefGenerator 白名单 | 结构上排除 MAILBOX/PYTHON 类参数，无需运行时分级 |
| 发送/接收角色 | 运行时按实体组件选择 | 编译期 `ProcessContext` 条件生成 Send/Receive/Forbidden stub | 错误路径在编译期消除，运行时无分支 |
| RPC 消息编码 | `ExposedMethodMessageRange` 一级+二级 subMsgID | 单一 uint32 RPC ID + payload | 协议简洁；无 ID 空间溢出问题（14b 类型 × 8b 方法 = 4.2M 方法） |

---

## 9. Phase 10 任务清单（解阻 Client↔Cell）

> 本节摘自原 `BIGWORLD_RPC_REFERENCE.md §13.3`。

### P0-1 定义 `ClientCellRpc` 消息结构

- **目标**：使客户端能向 CellApp 发送 cell_methods RPC。
- **文件**：`src/server/baseapp/baseapp_messages.h`（msg ID 2023 附近）
- **结构**：
  ```cpp
  struct ClientCellRpc {
    EntityID target_entity_id{0};      // self.id 或 AOI 内其他实体
    uint32_t rpc_id{0};                // 0xTTTT_DDNN 格式
    std::vector<std::byte> payload;    // 序列化后的参数
  };
  // 序列化：target_entity_id + rpc_id + payload(varint length + bytes)
  ```
- **验收**：单元测试 `test_client_cell_rpc_serde` 覆盖 round-trip。

### P0-2 实现 `BaseApp::OnClientCellRpc` handler

- **目标**：BaseApp 校验 + 转发到 CellApp；追加 `caller_entity_id`。
- **文件**：`src/server/baseapp/baseapp.cc`（参照 `OnClientBaseRpc` 的 2122-2148）
- **步骤**：
  1. Channel → `proxy_entity_id` 映射查找（沿用 `client_entity_index_`）
  2. `EntityDefRegistry::FindRpc(rpc_id)` → 校验存在 + 方向为 Cell
  3. `IsExposed(rpc_id)` 校验；若 `GetExposedScope == kOwnClient` 则强制 `target_entity_id == proxy_entity_id`；若 `kAllClients` 允许跨实体；若 `kNone` 拒绝
  4. 通过 CellAppMgr 查找 `target_entity_id` 所在 CellApp Channel
  5. 构造 `ClientCellRpcForward { caller_entity_id = proxy_entity_id, target_entity_id, rpc_id, payload }` 发送至 CellApp
- **安全要点**：`caller_entity_id` 由服务端写入，客户端 payload 不允许含此字段。
- **验收**：集成测试覆盖三种拒绝情形 + 合法转发。

### P0-3 建立 CellApp 骨架

- **目标**：CellApp 进程启动、注册到 CellAppMgr、接收消息。
- **文件**：
  - `src/server/cellapp/CMakeLists.txt`（添加 cellapp 二进制）
  - `src/server/cellapp/cellapp.h` / `cellapp.cc`
  - `src/server/cellapp/cellapp_messages.hpp`（msg ID 3000-3099 区间）
  - `src/server/cellapp/cellapp_entity.h` / `.cc`
- **cellapp_messages.hpp 定义**（Phase 10 版，不含 Real/Ghost）：
  | ID | 消息 | 方向 |
  |---|---|---|
  | 3000 | `CreateCellEntity` | BaseApp → CellApp |
  | 3002 | `DestroyCellEntity` | BaseApp → CellApp |
  | 3003 | `ClientCellRpcForward` | BaseApp → CellApp（含 caller_entity_id） |
  | 3004 | `InternalCellRpc` | BaseApp → CellApp（Base→Cell，不含 caller） |
- **验收**：`test_cellapp_startup` 跑通、`test_create_cell_entity` 创建/销毁 round-trip。

### P0-4 CellApp 侧 RPC 校验 + 分发

- **目标**：接收 RPC 并调用 C# 分发器，`OWN_CLIENT` 二次校验。
- **文件**：`src/server/cellapp/cellapp.cc` 新增 `OnClientCellRpcForward`
- **步骤**：
  1. 从消息提取 `caller_entity_id` / `target_entity_id` / `rpc_id` / `payload`
  2. 本地 `CellAppEntity` 索引查 `target_entity_id` 是否在此进程
  3. `GetExposedScope(rpc_id) == kOwnClient` 时断言 `caller_entity_id == target_entity_id`
  4. C# 回调：`DispatchCellRpc(target_entity_id, rpc_id, payload, len)` → `DefRpcDispatcher.Dispatch_*_CellRpc`
- **验收**：集成测试客户端 → BaseApp → CellApp 三跳，Cell 方法 Python 端（C#）正确执行。

### P0-5 DefGenerator 在 Client 上下文生成 ClientCellRpc Send stub

- **目标**：C# 客户端 `self.cell.useItem(...)` 发送正确消息。
- **文件**：`src/csharp/Atlas.Generators.Def/RpcStubEmitter.cs`
- **当前状态**：Send stub 已生成，但底层 `ClientNativeApi.SendCellRpc()` P/Invoke 或 C++ 接收侧缺失。
- **步骤**：
  1. 核对 `ClientNativeApi.SendCellRpc()` 签名是否传 `target_entity_id`（应默认为 `self.id`）
  2. 扩展生成器允许 `other.cell.method()` 形式（Phase 10 后期可选）
- **验收**：C# 客户端单元测试模拟发送 → C++ 侧序列化 round-trip 通过。

### P0-6 `self.base` 的 Receive 路径最小闭环（已完成，此处作基线验证）

- **状态**：✅ 已实现（`baseapp.cc:2122-2148`）。作为 P0 其他任务的回归基线，不再重复。

---

## 10. Phase 11 任务清单（CellApp→Client 扇出 + AOI + Ghost）

> 本节摘自原 `BIGWORLD_RPC_REFERENCE.md §13.4`。

### P1-1 Witness / RangeList 最小可用实现

- **目标**：CellApp 维护每实体的观察者列表，支持按距离剔除。
- **文件**：`src/server/cellapp/witness.h` / `range_list.h`
- **数据结构**：
  - `RangeList`：X/Z 轴双向链表，插入/移动 O(1) 本地；
  - `Witness`：每个有 Owner Client 的 Entity 持有；记录已见过的其他实体 `EntityCache`。
- **验收**：1000 实体场景下 `MoveEntity` 每 tick ≤ 1ms。

### P1-2 真正的 AOI 扇出 `BroadcastRpcFromCell`

- **目标**：Cell 调 `self.otherClients.m()` 时按 AOI 展开观察者。
- **文件**：
  - C#：`MailboxEmitter` 为 Cell 实体生成 `ownClient` / `otherClients` / `allClients` 三个属性（返回带 `MailboxTarget` 的 struct）
  - C++：CellApp `Entity::SendToClients(target_mode, rpc_id, payload)` 按观察者分包
  - BaseApp `OnBroadcastRpcFromCell` 按 `observer_entity_id → client_channel` 映射分发
- **步骤**：
  1. C# 生成 `self.allClients.m()` Send stub，带 `MailboxTarget` 参数
  2. CellApp 根据 MailboxTarget 枚举 Witness 列表：
     - `OwnerClient` → 直发 Owner
     - `OtherClients` → 遍历 AOI 内其他有 Owner 的实体
     - `AllClients` → Owner + AOI 其他
  3. 每个观察者生成一条 `SelfRpcFromCell` 发送给其 BaseApp Proxy
- **验收**：集成测试：A 调 `self.otherClients.showEffect()`，AOI 内 B/C 客户端收到、A 自身不收到。

### P1-3 DeltaSync 按 `PropertyScope` 过滤

- **目标**：属性更改按 `OwnClient` / `OtherClients` / `AllClients` 分离打包。
- **文件**：`src/csharp/Atlas.Generators.Def/DeltaSyncEmitter.cs`
- **步骤**：
  1. 为每个 Entity 生成三组 dirty mask：owner / others / all
  2. `SerializeOwnerDelta` / `SerializeOtherDelta` 分别过滤
  3. CellApp 在 Witness tick 末尾调用对应序列化、发送给观察者
- **验收**：`password`（`OWN_CLIENT`）变更仅 Owner 收到；`hp`（`ALL_CLIENTS`）所有观察者收到。

### P1-4 Real / Ghost 架构（Phase 11）

- **目标**：实体跨 CellApp 时 Ghost 透明转发 RPC 到 Real。
- **文件**：
  - `src/server/cellapp/cellapp_entity.h` 增加 `real_channel_`（Ghost 时非空）、`ghost_channels_[]`（Real 时列表）
  - `src/server/cellapp/message_handlers.cc` 所有 RPC handler 先检查 `is_real()`
- **消息级 reality 标记**：
  ```cpp
  // cellapp_messages.hpp
  ATLAS_CELLAPP_ENTITY_MSG(ClientCellRpcForward, REAL_ONLY);
  ATLAS_CELLAPP_ENTITY_MSG(InternalCellRpc,      REAL_ONLY);
  ATLAS_CELLAPP_ENTITY_MSG(GhostPositionUpdate,  GHOST_ONLY);
  ```
- **Ghost 转发**：对 REAL_ONLY 消息，Ghost 持有的 handler 调用 `real_channel_->bundle() << msg_id << payload` 转发；脚本层（C#）无感知。
- **验收**：
  1. 实体 Real 在 CellApp A、Ghost 在 CellApp B
  2. 客户端连接 BaseApp P，假设 BaseApp 把 RPC 发给 CellApp B
  3. Ghost 转发至 A，A 执行，结果返回客户端
  4. C# 方法只在 A 执行一次

### P1-5 Ghost 属性同步（GHOSTED Flag）

- **文件**：`src/server/cellapp/cellapp_entity.cc`（`on_ghosted_data_update` handler）+ DefGenerator
- **步骤**：
  1. DefGenerator 按 `PropertyScope` 判断是否需要 GHOSTED（规则：`CellPublic` / `OtherClients` / `AllClients` / `CellPublicAndOwn` → GHOSTED；其他 → 否）
  2. Real 属性变更时发 `GhostedDataUpdate` 给所有 Ghost
  3. Ghost 更新本地副本用于生成 Witness RPC
- **验收**：跨 CellApp 两实体互相可见、属性读取一致。

---

## 11. 安全加固路线图（Atlas 独有机会）

> 本节摘自原 `BIGWORLD_RPC_REFERENCE.md §13.5`。BigWorld 在这些维度没有内建能力；Atlas 借重构机会可一并填补。

### S-1 客户端 RPC 频率限制

- **位置**：`baseapp.cc` 外部消息入口（`OnClientBaseRpc` / `OnClientCellRpc`）
- **方案**：每 Proxy 一个 token bucket：
  ```cpp
  struct RpcRateBucket {
    uint64_t tokens{kBucketSize};
    uint64_t last_refill_us{0};
  };
  ```
- **配置**：全局默认 50 rpc/sec/proxy，允许 `.def` 级别方法覆盖。
- **超限行为**：丢弃 + 计数；连续超限 5 tick 断开连接。
- **验收**：单元测试发 100 RPC / 100ms → 被限流；正常速率不受影响。

### S-2 RPC payload 大小校验

- **位置**：`EntityDefRegistry::ValidateRpc` 前置检查
- **方案**：按 `RpcDescriptor.param_types` 估算最大序列化大小，超出则丢弃。
- **验收**：畸形超长 payload 不引起 C# 反序列化异常。
- **关联**：与本文档 §3 M-4（Payload 最大尺寸硬限制）配合落地。

### S-3 RPC ID 空间审计工具

- **目标**：避免多团队协作下 type_id 冲突。
- **文件**：`tools/rpc_id_audit/`（C# CLI 工具）
- **功能**：扫描所有 `.def` + 生成代码，检测：
  - 同一 entity type_id 两次分配
  - 同一 (direction, type, method) 三元组重复
  - exposed=None 的方法被 Client 上下文 stub 引用
- **接入**：CMake target `atlas_rpc_audit`，CI 阶段强制运行。
- **关联**：与 §2 H-3（Entity Type ID 跨构建稳定化）共用 CI 通路。

### S-4 Caller EntityID 篡改审计日志

- **目标**：当 CellApp 发现 `caller_entity_id != target_entity_id`（在 OWN_CLIENT 方法下）时记录安全事件。
- **实现**：CellApp 统计面板 + 按 Proxy 聚合次数；阈值后上报 BaseAppMgr。
- **价值**：能在客户端伪造尝试初期发现异常（BigWorld 无此能力）。

---

## 12. 任务依赖与验收里程碑

> 本节摘自原 `BIGWORLD_RPC_REFERENCE.md §13.6 ~ §13.7`。

### 12.1 任务依赖关系图

```
  P0-1 ClientCellRpc 结构 ──┐
                            ├─► P0-2 BaseApp handler ──┐
  P0-3 CellApp 骨架 ────────┼────────────────────────┼─► P0-4 CellApp 校验分发 ──► 【Client→Cell 端到端】
                            │                          │
  P0-5 Client Send stub ────┘                          │
                                                       ▼
                                               P1-1 Witness/RangeList
                                                       │
                                                       ▼
                                     ┌──── P1-2 AOI 扇出 ────┐
                                     │                        │
                                     ▼                        ▼
                                P1-3 DeltaSync 过滤      P1-4 Real/Ghost (Phase 11)
                                                              │
                                                              ▼
                                                         P1-5 Ghost 属性同步

  独立可并行：
    S-1 频率限制 │ S-2 payload 大小 │ S-3 ID 审计工具 │ S-4 篡改日志
```

### 12.2 验收里程碑

| 里程碑 | 依赖任务 | 验收标准 |
|---|---|---|
| M1 — Client↔Cell 打通 | P0-1 ~ P0-5 | C# Cell 方法从客户端触发执行，单 CellApp 单 Entity |
| M2 — 同 CellApp 多 Entity AOI | P1-1, P1-2 | 两玩家互相看见、`self.otherClients.m()` 仅对方收到 |
| M3 — 属性按 Scope 扇出 | P1-3 | `OwnClient` 属性仅 Owner 收、`AllClients` 全收 |
| M4 — 跨 CellApp 分布式 | P1-4, P1-5 | 一个 Space 分布两 CellApp，Real/Ghost 透明，脚本无感知 |
| M5 — 安全基线 | S-1 ~ S-4 | 客户端伪造/洪水测试均被拦截并记录 |

---

## 13. Atlas 设计注记（从 BigWorld 参考文档迁出的散落段落）

> 原 `BIGWORLD_RPC_REFERENCE.md` 中散落在各章节以 "**Atlas 对应**" / "**Atlas 机会**" 开头的说明段落，汇总于此。

### 13.1 `.def` 方法定义与 Exposed 标记（对应 BigWorld §2）

`.def` 文件中 `cell_methods` / `base_methods` 的 `<exposed>` 属性标记等价于 BigWorld 的 `<Exposed/>`。`.def` 的 `<client_methods>`/`<cell_methods>`/`<base_methods>` 节直接定义 RPC 方向，`exposed` 作为正交属性控制客户端可达性。DefGenerator 根据进程上下文（`ATLAS_BASE`/`ATLAS_CELL`/`ATLAS_CLIENT`）生成对应的 Send stub / Receive partial / Forbidden stub。

### 13.2 CLIENT_UNSAFE 类型警告对应（对应 BigWorld §4.3）

Atlas 的 Source Generator 在编译期强制类型安全，比 BigWorld 的加载时检查更早发现问题。C# 强类型 + DefGenerator 白名单结构性排除 MAILBOX/PYTHON 类参数。

### 13.3 SourceEntityID 身份验证（对应 BigWorld §4.5）

- **Client → Base**（`ClientBaseRpc` msg 2022，已实现）：BaseApp 通过 Channel ↔ EntityID 映射认证调用者，Proxy 就是 Base 实体本身，无需显式 `caller_entity_id`。
- **Client → Cell**（`ClientCellRpc` msg 2023，**消息 ID 占位、结构体未定义**）：规划方案是由 BaseApp 在转发至 CellApp 前追加 `caller_entity_id = proxy.entity_id()`，CellApp 对 `OWN_CLIENT` 方法校验 `target_entity_id == caller_entity_id`。客户端不可伪造此字段。详见 §9 Phase 10 任务 P0-1 / P0-2。

### 13.4 Rate limiting 的 Atlas 机会（对应 BigWorld §4.6）

BigWorld 在 Exposed 方法上没有调用频率限制、ACL、IP 白名单等更细粒度的权限系统。Atlas 可在 C++ 校验层加入 RPC 频率限制，这是 BigWorld 缺少的防护 — 详见 §11 S-1。

### 13.5 客户端可达性三重拦截（对应 BigWorld §5）

- **现状**：客户端 Mailbox 只会针对 Player 自身实体生成 Send stub（`ProcessContext.Client` 下 DefGenerator 只暴露 `self.cell/self.base` 路径）。跨实体的 `other.cell.method()` 形式尚未生成客户端代理，也无对应网络协议。
- **规划（Phase 10）**：对齐 BigWorld 的三分：
  1. `self.base.method()` → 允许，映射到 `ClientBaseRpc`
  2. `self.cell.method()` → 允许（exposed 任何变体），映射到 `ClientCellRpc` with `target_entity_id = self.id`
  3. `other.cell.method()` → 允许（仅 `ALL_CLIENTS`），映射到 `ClientCellRpc` with `target_entity_id = other.id`
  4. `other.base.method()` → 永久拒绝（架构层无网络路径）
- **三重拦截机制的 Atlas 等价实现**：
  - 第 1 层：C# Source Generator 在客户端上下文不生成 `other.base` 属性；
  - 第 2 层：`EntityDefRegistry` 校验 `RpcDescriptor.Direction() == Base` 且目标非 self → 拒绝；
  - 第 3 层：BaseApp `OnClientCellRpc` 对非 `ALL_CLIENTS` 方法强制 `target == proxy.id`。

### 13.6 Ghost 实体对应（对应 BigWorld §6）

所有项均为 **Phase 11 规划**，当前 CellApp 未实现（目录为空壳）。

| BigWorld | Atlas 规划 | 当前状态 | 说明 |
|---|---|---|---|
| `REAL_ONLY` 消息标记 | CellApp 消息 handler 检查 `Entity::is_real()` 并转发 | ❌ 未实现 | 消息级 reality 标志放在 `cellapp_messages.hpp` 的 handler table（Phase 11） |
| `pRealChannel_` 转发 | Ghost 持有 Real 所在 CellApp Channel | ❌ 未实现 | 需在 `CellAppEntity` 增加 `real_channel_` 字段 |
| `haunts_[]` 追踪 Ghost | Real 维护 Ghost CellApp 列表 | ❌ 未实现 | 边界跨越时由 CellAppMgr 协调 |
| 属性 `<Flags>` GHOSTED | `.def` 的 `<property scope="...">` + DefGenerator 按 `PropertyScope` 过滤 | ⚠️ 部分实现 | `PropertyScope` 枚举已定义（`DefModel.cs:12-22` / `entity_type_descriptor.h:29-38`），但 DeltaSync emitter 尚未按 scope 生成 per-observer 过滤 |
| `<DetailLevel>` LoD | C# `.def` 扩展 `<detail_level>` 节点 | ❌ 未实现 | 后置任务，不在 Phase 11 范围 |
| Controller `ControllerDomain` | Controller 子类指定域 | ❌ 未实现 | Controller 系统未设计，与 Phase 11 并行 |
| Ghost 透明转发 | 脚本层无感知 | ❌ 未实现 | **Phase 11 验收核心：C# 侧 `self.cell.method()` 无论 Real/Ghost 行为一致** |

### 13.7 方法索引体系对应（对应 BigWorld §7）

RPC ID 格式 `0xTTTT_DDNN` 已统一了方向和方法编号，不需要维护 BigWorld 那样的两套索引（exposed + internal）。Source Generator 在编译期生成 `RpcIds` 常量，客户端和服务端共享同一套 ID。

### 13.8 网络协议简化（对应 BigWorld §8）

Atlas 统一使用消息 ID + RPC ID 的二级编码，避免 BigWorld 的 `ExposedMethodMessageRange` 复杂编码。消息定义参见：
- `src/server/baseapp/baseapp_messages.hpp` — BaseApp 协议
- `src/server/cellapp/cellapp_messages.hpp` — CellApp 协议（Phase 10 建立）

---

## 附录 A：相关文件快速索引

| 关注点 | 主要文件 |
|---|---|
| RPC ID 生成 | `src/csharp/Atlas.Generators.Def/Emitters/RpcIdEmitter.cs` |
| Dispatcher 生成 | `src/csharp/Atlas.Generators.Def/Emitters/DispatcherEmitter.cs` |
| Mailbox 生成 | `src/csharp/Atlas.Generators.Def/Emitters/MailboxEmitter.cs` |
| 类型映射 | `src/csharp/Atlas.Generators.Def/DefTypeHelper.cs` |
| .def 解析 | `src/csharp/Atlas.Generators.Def/DefParser.cs` |
| 序列化 | `src/csharp/Atlas.Shared/Serialization/SpanWriter.cs` / `SpanReader.cs` |
| C++ Registry | `src/lib/entitydef/entity_def_registry.h` / `.cc` |
| BaseApp RPC 入口 | `src/server/baseapp/baseapp.cc`（`OnClientBaseRpc` 等） |
| 消息结构 | `src/server/baseapp/baseapp_messages.h` |
| C# ↔ C++ 桥接 | `src/csharp/Atlas.Runtime/Interop/NativeCallbacks.cs` / `NativeApi.cs` |
| 运行时核心 | `src/csharp/Atlas.Runtime/Core/EngineContext.cs` / `RpcBridge.cs` |
