# Entity Mailbox 代理机制

> **状态**:✅ 已落地。`Atlas.Generators.Def` 的 `MailboxEmitter` /
> `RpcStubEmitter` / `DispatcherEmitter` / `RpcIdEmitter` 全部在用,
> 由 `Atlas.Runtime.Tests` + `Atlas.Generators.Tests/DefGeneratorTests.cs`
> 覆盖;Provider 端 RPC 路由已在 BaseApp / CellApp 接通。

BigWorld 通过 Mailbox 在 Base / Cell / Client 三端做跨端透明 RPC:

```python
entity.client.showDamage(100, position)       # 拥有者客户端
entity.allClients.playAnimation("attack")     # 所有客户端
entity.otherClients.showEffect("slash")       # 其他客户端
entity.cell.moveTo(Vector3(10, 0, 20))        # 目标 Cell
entity.base.saveData()                        # 目标 Base
```

Python 版本依赖 `__getattr__` 动态代理 + 反射,属于 IL2CPP 禁区。Atlas 改
由 `entity_defs/*.def` + `Atlas.Generators.Def` 在编译期生成等价 API,
零反射、零堆分配。

## 1. 输入与生成物

`entity_defs/Avatar.def` 中 `<client_methods>` / `<cell_methods>` /
`<base_methods>` 节列出方向与方法签名;`<exposed>` 标记客户端可调用的
Cell 方法。`Atlas.Generators.Def` 为每个实体类型生成 partial 扩展。

各 Emitter 产出(目录 `src/csharp/Atlas.Generators.Def/Emitters/`):

| Emitter | 产出 |
|---|---|
| `MailboxEmitter` | `{Entity}ClientMailbox` / `{Entity}CellMailbox` / `{Entity}BaseMailbox` `readonly struct`;以及实体上的 `Client` / `AllClients` / `OtherClients` / `Cell` / `Base` 属性。`Atlas.Rpc.MailboxTarget` 提供 `OwnerClient = 0 / AllClients = 1 / OtherClients = 2` |
| `RpcStubEmitter` | Mailbox 方法体:用 `SpanWriter` 序列化参数后调用 `SendClientRpc / SendCellRpc / SendBaseRpc` |
| `DispatcherEmitter` | 接收端 `RpcDispatcher`,按 `(typeIndex, direction, methodIndex)` 反序列化参数,调用实体的 `OnXxx` 处理器 |
| `RpcIdEmitter` + `RpcIdEncoder` | RPC ID 常量表,packed 格式 `direction:2 | typeIndex:14 | methodIndex:8`(参见 `entity_type_registration.md` §3) |
| `RpcArgCodec` | 标量 / 容器 / Component / Struct 参数共用的编解码助手 |

## 2. 发送端调用链

```
entity.Client.ShowDamage(100f, pos)
   └─> new AvatarClientMailbox(entity, MailboxTarget.OwnerClient)
         └─> var writer = new SpanWriter(24);
             writer.WriteFloat(100f);
             writer.WriteVector3(pos);
             entity.SendClientRpc(RpcIds.Avatar_ShowDamage, writer.WrittenSpan);
               └─> NativeApi.AtlasSendClientRpc(EntityId, rpcId, target, payload)
                     └─> [LibraryImport("atlas_engine")]
                           └─> INativeApiProvider.SendClientRpc(...)
```

`ServerEntity.SendClientRpc(int rpcId, ReadOnlySpan<byte> payload)` 不再
携带 `MailboxTarget`;目标信息由 Mailbox struct 在 payload 中编码,或由
Provider 自身推断(例如 `AllClients` 走广播)。

## 3. 接收端调用链

```
C++ 网络层收到 RPC 消息
   └─> Provider 还原为 (entity_id, rpc_id, payload)
         └─> RpcBridge.Dispatchers[direction](entity, rpcId, ref reader)
               └─> RpcDispatcher.DispatchAvatar(entity, rpcId, ref reader)
                     └─> entity.OnShowDamage(reader.ReadFloat(), reader.ReadVector3())
```

## 4. 关键约束

1. **零分配目标**。`readonly struct` Mailbox + `ref struct SpanWriter`;
   写入缓冲由 `ArrayPool<byte>` 承载,`finally { writer.Dispose(); }`
   归还。
2. **`SpanReader` 是 `ref struct`**。不能穿越 `Action<>` / lambda /
   泛型参数;Dispatcher 生成代码直接 `ref` 传参。
3. **RPC ID 稳定性**。`.def` 中方法顺序变化会让 ID 漂移;
   `Atlas.Generators.Def` 的诊断会在客户端/服务端版本不匹配时告警,
   `EntityDefRegistry::PersistentPropertiesDigest` / 类型 schema 校验在
   握手时阻断不一致。
4. **`exposed` 检查**。客户端 → Cell 方向必须显式在 `.def` 里 `exposed`,
   否则 Provider 侧拒绝分发。

## 5. 关联

- [entity_type_registration.md](entity_type_registration.md) — RPC ID 编码与 `EntityDefRegistry` 元数据。
- [serialization_alignment.md](serialization_alignment.md) — 参数序列化字节格式。
- [../generator/def_generator_design.md](../generator/def_generator_design.md) — `.def` 文件格式与 DefGenerator 整体设计。
- [../bigworld_ref/BIGWORLD_RPC_REFERENCE.md](../bigworld_ref/BIGWORLD_RPC_REFERENCE.md) — BigWorld RPC 语义参考。
