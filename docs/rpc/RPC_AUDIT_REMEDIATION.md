# Atlas RPC 缺陷与安全加固清单

> 来源：2026-04 RPC 实现审计；本文档只跟踪 **当前仍未修复** 的结构性缺陷与待补的安全加固项。
> Phase 10/11（Client↔Cell + AoI 扇出 + Real/Ghost）已落地，实现以代码为准，不在本文档复述。
> 关联：[BigWorld RPC 参考](../bigworld_ref/BIGWORLD_RPC_REFERENCE.md)

---

## 0. 缺陷一览

| 编号 | 严重度 | 类别 | 主题 | 预估工作量 |
|---|---|---|---|---|
| C-1 | 🔴 Critical | 序列化 | `SpanWriter`/`SpanReader` 缺 int8/uint8 方法 | 15 min |
| C-2 | 🔴 Critical | RPC ID | `RpcIdEncoder.Encode` 位段无边界校验 | 30 min |
| C-3 | 🔴 Critical | RPC ID + 协议 | 方法索引按字母序漂移 + 无 EntityDef 握手 | 1–2 天 |
| C-4 | 🟠 High | 初始化 | 生成代码仍带 `[ModuleInitializer]`，顺序不保证 | 2 h |
| H-1 | 🟡 Medium | BaseApp | `OnCellRpcForward` 无实体存活检查 / 无 WARN | 30 min |
| H-2 | 🟡 Medium | Dispatcher | switch default 分支无日志，未知 RPC 静默 | 20 min |
| H-3 | 🟠 High | RPC ID | Entity Type ID 仍按字母序自动分配 | 半天 |
| M-1 | 🟡 Medium | 观测 | Dispatcher 反序列化错误无 entity/method 上下文 | 1 h |
| M-2 | 🟡 Medium | 生命周期 | `DispatchRpc` 不检查 `entity.IsDestroyed` | 30 min |
| M-3 | 🟢 Low | 重入 | RPC 递归深度无防护 | 1 h |
| M-4 | 🟡 Medium | 协议 | RPC Payload 无最大尺寸限制 | 1 h |
| L-1 | 🟢 Low | 观测 | 无 RPC 计数指标 | 半天 |
| L-2 | 🟢 Low | 观测 | 无 trace ID 贯通 | 半天 |
| S-1 | 🟠 High | 安全 | BaseApp 入口无 RPC 频率限制 | 1 天 |
| S-3 | 🟢 Low | 工具 | 缺 `tools/rpc_id_audit/` 跨构建审计工具 | 半天 |
| S-4 | 🟢 Low | 安全 | 无 caller 篡改审计日志 | 半天 |

合计：Critical ~2-3 天 / High ~1-2 天 / Medium ~3 天 / 其他随排期。

---

## 1. RPC ID wire layout（实现现状）

`src/csharp/Atlas.Generators.Def/Emitters/RpcIdEncoder.cs` 当前编码：

```
bit  31      kReplyBit       1 = sender awaits an EntityRpcReply
bits 24-30   slot_idx        0 = entity body, >0 = component slot
bits 22-23   direction       0=Client, 2=Cell, 3=Base; 1 reserved
bits  8-21   typeIndex       1-based, currently alphabetic (see C-3 / H-3)
bits  0-7    methodIdx       1-based, currently alphabetic (see C-3)
```

C-2 / C-3 / H-3 都围绕这套布局：编码端无任何范围校验，索引来源仍是名字字母序，删除任一字段即会让既存调用者沉默错位。

---

## 2. Critical 缺陷

### C-1 补齐 int8 / uint8 序列化方法

**现象** — `Atlas.Shared/Serialization/SpanWriter.cs` 与 `SpanReader.cs` 没有 `WriteInt8`/`WriteUInt8`/`ReadInt8`/`ReadUInt8`，但 `DefTypeHelper` 仍把 `<arg type="int8"/>`/`uint8` 映射到这四个方法。任何在 `.def` 用单字节签名整型的 RPC 立刻编译失败。

**修复**

`SpanWriter.cs`（紧邻 `WriteByte`）：
```csharp
public void WriteInt8(sbyte value) => WriteByte((byte)value);
public void WriteUInt8(byte value) => WriteByte(value);
```

`SpanReader.cs`（紧邻 `ReadByte`）：
```csharp
public sbyte ReadInt8() => (sbyte)ReadByte();
public byte ReadUInt8() => ReadByte();
```

**验收** — `tests/csharp/Atlas.Shared.Tests` 加 round-trip `[Theory]`；临时构造 `int8` 参数的 `.def`，DefGenerator 产物可通过 `dotnet build`。

---

### C-2 RpcIdEncoder 加位段边界校验

**现象** — `RpcIdEncoder.cs:13-17` 直接位移合成 ID，对 `slot >= 0x80`、`typeIndex > 0x3FFF`、`methodIdx > 0xFF` 全部沉默。两个不同方法可能被分到同一 ID，调 A 路由到 B。

**修复** — 在 `Encode` 内 assert：
```csharp
public static int Encode(int slot, byte direction, ushort typeIndex, int methodIdx)
{
    if ((uint)slot > 0x7F)
        throw new InvalidOperationException($"slot={slot} exceeds 7-bit limit");
    if (typeIndex > 0x3FFF)
        throw new InvalidOperationException(
            $"typeIndex={typeIndex} exceeds 14-bit limit; max 16383 entity types");
    if ((uint)methodIdx > 0xFF)
        throw new InvalidOperationException(
            $"methodIdx={methodIdx} exceeds 8-bit limit; max 255 methods per direction");
    if ((uint)direction > 0x3)
        throw new InvalidOperationException($"direction={direction} exceeds 2 bits");
    return (slot << 24) | (direction << 22) | (typeIndex << 8) | methodIdx;
}
```

C++ 侧在 `entitydef/entity_type_descriptor.h` 的 RPC 注册路径加对称 `ATLAS_FATAL` / `static_assert`。

**验收** — 单元测试覆盖各位段最大值 / 最大值+1（应抛）；正常规模无变化。

---

### C-3 方法索引稳定化 + EntityDef Digest 握手

**现象**
1. `RpcIdEmitter.cs:43` / `DispatcherEmitter.cs:203,291` / `RpcStubEmitter.cs:48` / `MailboxEmitter.cs` 仍按 `OrderBy(m => m.Name)` 字母序分配 `methodIdx`。在 `.def` 新增字母序靠前的方法，所有靠后方法 ID 全部右移 1 — 旧客户端调 A 实际命中新 B，零日志、零异常。
2. `LoginRequest` 中无 `entity_def_digest` 字段（grep `EntityDefDigest` / `entity_def_digest` 全仓零结果），客户端/服务端 `.def` 不一致时无任何启动期拦截。

**修复 — Part A：methodIdx 按声明顺序**

`DefParser` 把方法存入 `MethodDefModel` 时已是 `XElement.Elements("method")` 顺序；只需移除 emitter 端所有 `OrderBy(m => m.Name)`，全仓共 7 处：

- `RpcIdEmitter.cs:43`
- `DispatcherEmitter.cs:203`（实体级 dispatch）
- `DispatcherEmitter.cs:291`（组件级 dispatch）
- `RpcStubEmitter.cs:48`
- `MailboxEmitter.cs:102`
- `TypeRegistryEmitter.cs:152`（C++ 侧元数据注册顺序）
- `ComponentEmitter.cs:379`

`MethodDefModel` 增 `DeclarationIndex`（0-based）由 DefParser 写入，下游所有 emitter 改用此字段。这样未来若有 reorder 工具或 IDE 自动整理，索引仍稳定。

**修复 — Part B：EntityDef Digest 启动握手**

构造规范化字符串（按 entity name 排序，每 entity 内方向固定 Client/Cell/Base，每方向方法按 **声明顺序**，参数仅类型名）：
```
Entity:Avatar
  Client:showEffect:uint32
  Cell:useItem[own_client]:uint32
  Base:logOff[own_client]:
```
对该字符串取 SHA-256，写入：
- C# 端 `Atlas.Rpc.EntityDefDigest.Sha256`（DefGenerator 生成）
- C++ 端 `EntityDefRegistry::digest()`（注册阶段计算并缓存）

`login_messages.h` 的 `LoginRequest` 增 `std::array<uint8_t, 32> entity_def_digest{};`；`loginapp.cc` 的登录处理在密码校验前比对，不一致返回 `LoginError::kDefMismatch`。客户端 SDK 在 `Connect()` 时自动带上常量。

**验收**
- `.def` 中间插入新方法 → 既有方法的生成 ID 不变（diff `RpcIds.g.cs`）。
- 客户端/服务端构建不同 `.def` → 登录立即被拒并返回清晰错误码。
- 同一 `.def` 两次独立构建 digest 字节级一致（CI 回归）。

---

## 3. High 缺陷

### C-4 显式化 EngineContext 初始化顺序

**现状** — `Atlas.Runtime/Core/EngineContext.cs:36-47` 已有 `Initialize()` 入口，但 4 个生成代码 emitter 仍各自打 `[ModuleInitializer]`：

- `DispatcherEmitter.cs:81` — `RegisterDefDispatchers()`
- `TypeRegistryEmitter.cs:38` — `RegisterAll()`（C++ 侧 entity 元数据）
- `FactoryEmitter.cs:32` — entity factory 注册
- `StructRegistryEmitter.cs:35` — struct 元数据注册

`[ModuleInitializer]` 之间无相对顺序保证。若 `Dispatcher` 先于 `TypeRegistry` 跑，C++ `FindRpc` 全空 → 首波 RPC 全拒；反过来在并发场景下首个 RPC 可能命中 `RpcBridge.Dispatchers[dir] == null` → 静默丢弃。Factory 与 StructRegistry 同样有先后依赖。

**修复**

1. 删除四处 `[System.Runtime.CompilerServices.ModuleInitializer]` 标注，方法保持 `internal static`。
2. `EngineContext.Initialize()` 显式调用，顺序固定：
   ```csharp
   public static void Initialize()
   {
       if (_initialized) throw new InvalidOperationException("already initialized");
       NativeCallbacks.Register();
       Atlas.Rpc.DefStructRegistry.RegisterAll();      // structs first — entities reference them
       Atlas.Rpc.DefEntityTypeRegistry.RegisterAll();  // entity metadata into C++ registry
       Atlas.Rpc.DefEntityFactory.RegisterAll();       // C# factory before any RestoreEntity callback
       Atlas.Rpc.DefRpcDispatcher.RegisterDefDispatchers();  // last — RPC routing depends on the above
       var prefix = NativeApi.GetProcessPrefix();
       EntityManager.Instance.SetProcessPrefix(prefix);
       _initialized = true;
   }
   ```
3. `RpcBridge.Dispatchers` 首次 dispatch 前 assert `_initialized`，开发期立刻暴露漏调链路。

**验收** — 启动日志固定顺序；多线程同时调 `Initialize` 仍只执行一次。

---

### H-3 Entity Type ID 跨构建稳定化

**现状** — `DefGenerator.cs:113-117` 仍：
```csharp
var allNames = defs.Select(d => d.Name).OrderBy(n => n).ToList();
for (int i = 0; i < allNames.Count; i++)
    typeIndexMap[allNames[i]] = (ushort)(i + 1);
```
`DefParser.cs:80-82` 已经解析了 `<entity name="..." id="...">` 写入 `EntityDefModel.ExplicitTypeId`，但 `DefGenerator` 没用。删除一个中间实体即让其它实体 `type_id` 整体左移；DBApp 用 `type_id` 持久化时会读错位、客户端/服务端 type_id 错配。

**修复**

1. 让 `<entity id="...">` 必填，由 `DefParser` 校验：缺失或非整数即 `Diagnostic.Error`。
2. `DefGenerator` 用 `def.ExplicitTypeId` 替代字母序映射，并校验全局唯一。
3. 集中清单 `entity_defs/entity_ids.xml`（与既有 `.def` 同目录）：
   ```xml
   <entity_ids>
     <entity name="Avatar" id="1"/>
     <entity name="Account" id="2"/>
     <entity name="_removed_boss" id="3" deprecated="true"/>
   </entity_ids>
   ```
   删实体只标 `deprecated="true"`，禁止 id 复用。
4. CI 钩子（见 S-3）。

**验收** — 缺 id / id 重复 / id 回收 都 fail；删中间实体后剩余实体 type_id 不变。

---

### S-1 客户端 RPC 频率限制

**位置** — `baseapp.cc` 外部消息入口（`OnClientBaseRpc`、`OnClientCellRpc`）。

**方案** — 每 Proxy 一个 token bucket：
```cpp
struct RpcRateBucket {
  uint64_t tokens{kBucketSize};
  uint64_t last_refill_us{0};
};
```

全局默认 50 rpc/sec/proxy，允许 `.def` 方法级覆盖。超限：丢弃 + 计数；连续超限 5 tick 断开。配合 S-4 的篡改审计同一通路上报。

**验收** — 单元测试：100 RPC / 100 ms 触发限流；正常速率（10 rpc/s）零丢弃。

---

## 4. Medium 缺陷

### H-1 `OnCellRpcForward` 补实体存活检查

**现状** — `baseapp.cc:828-837` 直接 `dispatch_fn(msg.entity_id, ...)`，C# 侧 `NativeCallbacks.cs:321-325` 有 `entity is null` 兜底但 C++ 无 WARN，运维只看到管理面板而不知道是哪条转发命中已销毁实体。

**修复** — 加前置查表：
```cpp
void BaseApp::OnCellRpcForward(Channel& ch, const baseapp::CellRpcForward& msg) {
  if (entity_mgr_.Find(msg.entity_id) == nullptr) {
    ATLAS_LOG_WARNING("BaseApp: CellRpcForward for unknown entity {} (rpc_id=0x{:06X})",
                      msg.entity_id, msg.rpc_id);
    return;
  }
  // ... existing dispatch ...
}
```

**验收** — 集成测试触发 → 日志包含 WARN，无崩溃，C# 不被调用。

---

### H-2 Dispatcher switch default 加日志

**现状**
- 顶层 `Dispatch{dirName}` 的 `switch (target)`（`DispatcherEmitter.cs:141,143,164`）三处 default 全部 `break;` 或 `_ = rpcId; _ = reader;`，无日志。
- **更糟**：`Dispatch_{className}_{dirName}` 的 `switch (rpcId)`（`DispatcherEmitter.cs:200`）连 `default:` 分支都没有 — 未知 rpcId 直接 fall-through 跳过整个 switch，零日志、零异常。
- `Dispatch_{componentTypeName}_{dirName}` 的 `switch (methodIdx)`（`DispatcherEmitter.cs:288`）同样无 default。

版本不齐时表现是 "RPC 没反应"，且无任何 grep 线索。

**修复** — 五处 switch 都补上带日志的 default：
```csharp
default:
    Atlas.Log.Warning(
        "DefRpcDispatcher: unknown {0} for type {1} (rpc_id=0x{2:X8})",
        "{dirName}", target.GetType().Name, rpcId);
    break;
```
（`{dirName}` 在 emitter 字符串里展开）

**验收** — 单元测试给一个不存在的 rpcId → log sink 收到 Warning 一次。

---

### M-1 Dispatcher 反序列化错误带上下文

**现状** — `RpcArgCodec.EmitRead` 抛出的 "Insufficient data" 不含实体名/方法名/rpc_id；`NativeCallbacks.DispatchRpc` 顶层 catch 只看到栈顶。

**修复** — `DispatcherEmitter` 把每个 case 包进 try/catch 抛 `RpcDispatchException(entityName, methodName, rpcId, inner)`；`NativeCallbacks.DispatchRpc` 单独识别该异常类型记录到 `ErrorBridge`。

**验收** — 畸形 payload 时日志一行包含：方向 / 实体类型名 / 方法名 / rpc_id / 原始异常消息。

---

### M-2 Dispatcher 检查 `entity.IsDestroyed`

**现状** — `NativeCallbacks.cs:320-325` 只检查 `entity is null`。`ServerEntity.IsDestroyed = true` 的实体引用仍在 `EntityManager`，会被当作有效实体 dispatch，脚本层可能操作到已被清理的状态。

**修复**
```csharp
if (entity is null) { Log.Warning(...); return; }
if (entity.IsDestroyed) {
    Log.Debug($"DispatchRpc: dropping for destroyed entity {entityId}");
    return;
}
```

**验收** — 标记 `IsDestroyed = true` 后再投递 → 方法未被调用、Debug 日志记录一次。

---

### M-4 RPC payload 最大尺寸硬限制

**现状** — `ClientBaseRpc` / `ClientCellRpc` 等的 `payload` 字段只受帧长限制（MB 级）。恶意客户端发 10 MB string 触发 C# LOH 分配 + GC 抖动。

**修复**

`entitydef/entity_def_config.h` 新增：
```cpp
inline constexpr size_t kMaxRpcPayloadBytes = 64 * 1024;
```

`baseapp.cc:OnClientBaseRpc` / `OnClientCellRpc` 在 `FindRpc` 之后立刻：
```cpp
if (msg.payload.size() > kMaxRpcPayloadBytes) {
  ATLAS_LOG_WARNING("RPC payload {} bytes exceeds max from channel {}",
                    msg.payload.size(), ch.RemoteAddress());
  return;
}
```

按方法级覆盖（`.def` `<method max_payload="128K"/>`）作为后续可选增强；与 S-1 的 rate limiter 同一处级联（超限多次 → 主动断连）。

**验收** — 65 KB payload 被丢弃 + WARN，连接保持；合法请求不受影响。

---

## 5. Low / 长期加固

### M-3 RPC 递归深度保护

**现状** — `Atlas.Runtime/Core/ThreadGuard.cs` 只做主线程校验，无 RPC 调用栈深度计数。当前所有 RPC 走网络（异步 dispatch）暂无可触发路径，但未来 in-process loopback / 单元测试场景隐患存在。

**修复** — `ThreadGuard` 增 `[ThreadStatic] _rpcDepth` 与 `RpcScope : IDisposable`，`DispatchRpc` 用 `using var _ = new ThreadGuard.RpcScope();` 包裹；上限 32。

---

### L-1 / L-2 观测加固

- **L-1**：`atlas_rpc_received_total{direction, entity_type, method}` / `atlas_rpc_dispatch_failed_total{reason}` / `atlas_rpc_exposed_violation_total`，经 BaseAppMgr status endpoint 暴露。
- **L-2**：RPC 消息可选 `uint64 trace_id`（0 = 未启用）。BaseApp → CellApp → Client 全程透传，C# 入口写入 `AsyncLocal<long>`。

---

### S-3 RPC ID 跨构建审计 CLI

**目标** — 多人协作下避免 `type_id` / `method_id` 静默漂移。

**实现** — `tools/rpc_id_audit/audit.py`（按项目"工具一律 Python + `tools/bin/` 包装"惯例），或 `Atlas.Tools.RpcIdAudit` C# CLI，扫描 `.def` + 当前生成代码并对比上次提交的 `RpcIds.g.cs`：
- 同一 entity `type_id` 重复或回收 → fail
- `(direction, type, method)` 三元组重复 → fail
- 同名方法 ID 与上次构建不一致 → fail
- `exposed=None` 的方法被 Client 上下文 stub 引用 → fail

接入 CMake target `atlas_rpc_audit`，CI `clang-format` 后强制运行。与 H-3 的 `entity_ids.xml` 共用清单。

---

### S-4 caller 篡改审计日志

**位置** — `cellapp.cc:OnClientCellRpcForward`（已校验 OWN_CLIENT 时 source==target，仅 WARN）。

**加强** — 按 source proxy 聚合违规计数；超过阈值（如 1 分钟内 10 次）上报 BaseAppMgr，由后者关联 S-1 的限流器决定是否断连。

---

## 6. 修复顺序与提交建议

### 阶段 1 — Critical（合计 ~2-3 天，先合）

1. **C-1**（15 min）独立 PR：`fix(rpc): add int8/uint8 serialization to SpanWriter/Reader`
2. **C-2**（30 min）独立 PR：`feat(gen): validate RPC ID bit-field bounds in RpcIdEncoder`
3. **C-4**（2 h）独立 PR：`refactor(runtime): explicit EngineContext.Initialize order`
4. **C-3** 拆两 PR：
   - `refactor(gen): allocate methodIdx by declaration order`
   - `feat(net): add EntityDef digest handshake on login`

### 阶段 2 — High（与近期任务并行，~1-2 天）

5. **H-3**（半天）`feat(gen): require explicit entity id in .def files`
6. **S-1**（1 天）`feat(baseapp): per-proxy RPC rate limiter`

### 阶段 3 — Medium（随就近变更携带）

7. H-1 / H-2 / M-1 / M-2 / M-4 — 每条独立小 PR，配套单测。

### 阶段 4 — Low / 加固

8. M-3 / L-1 / L-2 / S-3 / S-4 — 视排期，不阻塞主线。

---

## 7. 快速验收 checklist

每项修复合入前必须通过：

- [ ] 改动文件只限修复范围，无附带重构
- [ ] 单元测试覆盖正常路径 + 至少一条边界 / 错误路径
- [ ] `ctest --build-config Debug --label-regex unit` 全绿
- [ ] `clang-format --dry-run --Werror` 零违规
- [ ] C# 侧 `dotnet build -warnaserror` 零警告
- [ ] commit message 引用对应编号（例：`Fixes RPC-AUDIT C-1`）
