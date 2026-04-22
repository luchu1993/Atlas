# 设计: Entity Mailbox 代理机制

> 归属阶段: ScriptPhase 4 (`Atlas.Generators.Def`) | 关联: ScriptPhase 3 (`ClrScriptEngine`)
>
> **状态（2026-04-18）：✅ 已落地**。输入来源从 C# Attribute 调整为 `entity_defs/*.def` 文件；发射器统一在 `src/csharp/Atlas.Generators.Def/Emitters/` 下。

---

## 1. 背景

BigWorld 通过 **Mailbox** 在 Base / Cell / Client 三端做跨端透明 RPC：

```python
entity.client.showDamage(100, position)       # 拥有者客户端
entity.allClients.playAnimation("attack")     # 所有客户端
entity.otherClients.showEffect("slash")       # 其他客户端
entity.cell.moveTo(Vector3(10, 0, 20))        # 目标 Cell
entity.base.saveData()                        # 目标 Base
```

Python 版本依赖 `__getattr__` 动态代理，运行时反射——属于 IL2CPP 禁区。

## 2. 现行方案：`.def` 驱动 + Source Generator

### 2.1 输入

`entity_defs/Avatar.def` 中 `<client_methods>` / `<cell_methods>` / `<base_methods>` 节列出方向和方法签名；`<exposed>` 标记客户端可调用的 Cell 方法。`Atlas.Generators.Def` 扫描这些节点，为每个实体类型生成 C# partial class 扩展。

### 2.2 生成物（由对应 Emitter 输出）

| 产出 | Emitter | 说明 |
|------|---------|------|
| `{Entity}ClientMailbox` / `{Entity}CellMailbox` / `{Entity}BaseMailbox` `readonly struct` | `MailboxEmitter.cs` | 一端一种代理；零堆分配，方法可内联 |
| 方法体：参数序列化 + `SendClientRpc / SendCellRpc / SendBaseRpc` 调用 | `RpcStubEmitter.cs` | 使用 `Atlas.Shared` 的 `SpanWriter` |
| 实体上的 `Client` / `AllClients` / `OtherClients` / `Cell` / `Base` 属性 | `MailboxEmitter.cs` | 分别 new 出上述 struct；`Atlas.Rpc.MailboxTarget` 提供 `OwnerClient = 0 / AllClients = 1 / OtherClients = 2` |
| `RpcDispatcher` switch（接收端） | `DispatcherEmitter.cs` | 按 `(typeIndex, direction, methodIndex)` 反序列化参数，调用实体的 `OnXxx(...)` 处理器 |
| `RpcIds` 常量表 | `RpcIdEmitter.cs` | 格式 `0xTTTT_DDNN`：`TypeIndex(16)` / `Direction(8, 0=Client / 2=Cell / 3=Base)` / `MethodIndex(8)` |

### 2.3 发送端调用链

```
entity.Client.ShowDamage(100f, pos)
   └─> new AvatarClientMailbox(entity, MailboxTarget.OwnerClient)
         └─> var writer = new SpanWriter(24);
             writer.WriteFloat(100f);
             writer.WriteVector3(pos);
             entity.SendClientRpc(RpcIds.Avatar_ShowDamage, writer.WrittenSpan);
               └─> NativeApi.SendClientRpc(EntityId, (uint)rpcId, payload)
                     └─> [LibraryImport("atlas_engine")] AtlasSendClientRpc
                           └─> INativeApiProvider.SendClientRpc(...)
```

`ServerEntity` 的 `SendClientRpc(int rpcId, ReadOnlySpan<byte> payload)` 签名里不再带 `MailboxTarget`；目标信息由 Mailbox struct 在序列化前选择性写入 payload，或由 Provider 自己推断（例如 `AllClients` 走广播）。具体策略由生成代码与 Provider 实现共同决定。

### 2.4 接收端调用链

```
C++ 网络层收到 rpc 消息
   └─> 经 Provider 还原为 (entity_id, rpc_id, payload)
         └─> RpcBridge.Dispatchers[direction](entity, rpc_id, ref reader)
               └─> RpcDispatcher.DispatchAvatar(entity, rpc_id, ref reader)
                     └─> entity.OnShowDamage(reader.ReadFloat(), reader.ReadVector3())
```

## 3. 关键约束

1. **零分配目标**：`readonly struct` + `ref struct SpanWriter`；写入缓冲由 `ArrayPool<byte>` 承载，`finally { writer.Dispose(); }` 归还。
2. **`SpanReader` 是 `ref struct`**：不能穿越 `Action<>` / lambda / 泛型参数；Dispatcher 生成的代码直接 `ref` 传参。
3. **RPC ID 稳定性**：`.def` 中方法顺序变化会导致 ID 漂移；DefGenerator 诊断会在客户端/服务端版本不匹配时告警。
4. **`exposed` 检查**：客户端 → Cell 方向的调用必须显式在 `.def` 里 `exposed`，否则 Provider 侧拒绝分发。

## 4. 关联

- [script_phase4_shared_generators.md](script_phase4_shared_generators.md) — Emitter 架构。
- [DEF_GENERATOR_DESIGN.md](../generator/DEF_GENERATOR_DESIGN.md) — `.def` 文件格式与 DefGenerator 整体设计。
- [DEFGEN_CONSOLIDATION_DESIGN.md](../generator/DEFGEN_CONSOLIDATION_DESIGN.md) — Entity/Rpc Generator 合并入 DefGenerator 的历程。
- [BIGWORLD_RPC_REFERENCE.md](../bigworld_ref/BIGWORLD_RPC_REFERENCE.md) — BigWorld RPC 语义参考。
- [serialization_alignment.md](serialization_alignment.md) — 参数序列化字节格式。
