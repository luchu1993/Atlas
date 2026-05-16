# Codegen 设计

## 现状

- `.def` 是 XML(`entity_defs/*.def`)
- 已有 codegen `Atlas.Generators.Def` 是 C# Roslyn Source Generator,~4500 行
- C++ 服务端通过 `EntityDefRegistry` 在运行时读 ATDF 二进制描述符,**不**解析 `.def`
- 已有先例 `Atlas.Tools.DefDump`:offline 工具,从编译产物抽 ATDF

## UE 客户端策略

走 **registry-driven + 类型化薄包装** 混合范式:

- decode/dispatch:`FClientEntityBase::ApplyDelta` 通用一份,通过 `EntityDefRegistry` 查表逐字段 decode
- 对外 API 类型化:codegen 生成 `Avatar.gen.h` 提供 `E->Health()` 而非 `E->GetField<float>("health")`

## CppEmitter 工具

新工具 `src/csharp/Atlas.Tools.CppEmitter/`,与 `Atlas.Tools.DefDump` 同级:

- C# 写,`dotnet run` 调用
- 直接读 `.def` XML,复用从 `Atlas.Generators.Def` 抽出的 `Atlas.Defs.Parser` lib
- 输出到 `samples/mvp/UEClient/Plugins/AtlasUE/Source/AtlasUE/gen/`
- `tools/bin/cpp_emit.bat` / `.sh` 提供薄包装

为什么不在 `Atlas.Generators.Def` 里直接 emit C++:Roslyn Source Generator 设计上 emit `.cs`,emit `.h/.cpp` 是反模式,而且阻塞 C# 编译。独立工具更干净。

## 输出物

### Layer 1(必生成,纯 C++)

| 输出 | 内容 |
|------|------|
| `<EntityName>.gen.h/cc` | entity wrapper(class `FAvatarEntity`)+ 类型化 accessor + 类型化 outgoing RPC + `ApplyDelta`/`DispatchRpc` override |
| `<ComponentName>.gen.h/cc` | `.def <component>` 对应纯 C++ 逻辑组件类 |
| `I<EntityName>View.gen.h` | typed view 接口,继承 `IEntityView`,类型化 incoming RPC 虚方法 + per-field 通知槽 |
| `<StructName>.gen.h/cc` | `.def <types>` 的 struct/alias 对应 C++ struct + `Serialize`/`Deserialize` |

### Layer 2(必生成,UE 适配)

| 输出 | 内容 |
|------|------|
| `UAtlasAvatarView.gen.h/cpp` | UCLASS,entity 在 BP 侧的**唯一**暴露面;含 entity 顶层 + logic component 字段/RPC 平铺 |
| `FAtlasUEAvatarView.gen.h/cpp` | view bridge,实现 `IAvatarView`,转发到 UCLASS 的 BP delegate |

## 几个 codegen 决策

| 决策 | 取值 | 备注 |
|------|------|------|
| Position 字段 | 走 volatile envelope `0xF001`,不参与 `ApplyReplicatedDelta` | 沿用 `ATLAS_DEF008`,与 C# 端一致 |
| 属性 BP 暴露 | 默认 getter+registry 查表;hot 字段未来扩 `@bp_cache` 标注 | M1 落地时确认是否需要 cache |
| 嵌套类型 USTRUCT 生成 | 是 | BP 直读容器元素必需 |
| RPC reply(Latent UFUNCTION) | 不做(本期) | wire 协议支持 `trace_id + reply_bit`,但 BP latent 暴露非必需 |
| Logic component 暴露 | 平铺到 `UAtlasAvatarView`,不为 component 生成独立 UClass | 见 [architecture.md](architecture.md) |

## 构建链路

```
.def + entity_ids.xml
     │
     ▼
[CppEmitter]  (dotnet run, offline)
     │
     ▼
samples/mvp/UEClient/Plugins/AtlasUE/Source/AtlasUE/gen/*.{h,cpp}
     │
     ▼
[UBT]  ←— 链接 atlas_net_client.dll (CMake 产出)
```

入口脚本:`tools/bin/build_mvp_ue.bat`(类比 `build_mvp_unity.bat`)。
