# Atlas RPC 审计修复（已全部落地）

> 2026-04 RPC 审计列出的所有缺陷与安全加固项已合入主线。本文档保留作为 RPC ID / 协议字段背景速查。
> 实现以代码为准；已修条目的 git log 是最终真相。
> 关联：[BigWorld RPC 参考](../bigworld_ref/BIGWORLD_RPC_REFERENCE.md)

---

## RPC ID wire layout

`src/csharp/Atlas.Generators.Def/Emitters/RpcIdEncoder.cs`：

```
bit  31      kReplyBit       1 = sender awaits an EntityRpcReply
bits 24-30   slot_idx        0 = entity body, >0 = component slot
bits 22-23   direction       0=Client, 2=Cell, 3=Base; 1 reserved
bits  8-21   typeIndex       1-based, taken from entity_defs/entity_ids.xml
bits  0-7    methodIdx       1-based by .def declaration order
```

`Encode` 对四个字段范围 assert；`<entity name="..."/>` 编译期必须出现在 `entity_ids.xml` 中（DEF019/024）；同次构建去重 (DEF021)；C++ `EntityDefRegistry::RegisterType` 运行时跨程序集 name/type_id 重复触发 `DefaultAssertHandler` fatal。

新分配 id 用 `tools/bin/def_id.{bat,sh} alloc <Name>`；CI 用 `def_id.py audit` 阻止已存在 id 改值或被删。

## 启动握手

客户端 `LoginRequest.entity_def_digest = Atlas.Rpc.EntityDefDigest.Bytes`，BaseApp 在 `OnPrepareLogin` 比对，不一致返回 `LoginStatus::kDefMismatch`。

## RPC 消息字段

每个 RPC 消息（`ClientBaseRpc` / `ClientCellRpc` / `ClientRpcEnvelope` / `CellRpcForward` / `BroadcastRpcFromCell` / `cellapp::ClientCellRpcForward` / `cellapp::InternalCellRpc`）末尾带 `uint64 trace_id`。`0` = 未追踪，由 `Atlas.Diagnostics.TraceContext.BeginInbound` 在接收边界自动 mint 一个 snowflake id；非零透传。

`Atlas.Log.{Info/Warning/Error}` 在 `TraceContext.Current != 0` 时自动前置 `[trace=...]`。
