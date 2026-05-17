# Codegen 设计

## 现状

- `.def` 是 XML(`entity_defs/*.def`)
- 已有 codegen `Atlas.Generators.Def` 是 C# Roslyn Source Generator,~4500 行,为 `Atlas.Client` / `Atlas.Mvp.Client` 产生 C# 实体类
- C++ 服务端通过 `EntityDefRegistry` 在运行时读 ATDF 二进制描述符,**不**解析 `.def`
- 已有先例 `Atlas.Tools.DefDump`:offline C# 工具,从编译产物抽 ATDF blob 给 DBApp 用

## 边界

**Codegen 只产纯 C++ 逻辑类型**,等价于 C# 端 `Atlas.Generators.Def` 给 `Atlas.Mvp.Client.Avatar` 生成的内容。

UE 引擎层(UCLASS / UPROPERTY / USTRUCT / BlueprintCallable / 任何 BP 暴露)**不在 codegen 范围**——那是游戏侧手写的 view 层(类似 `FAtlasUEActorView`),需要 BP 表面时游戏程序员自行定义 `UAtlasXxxView : UActorComponent` 并 push entity 字段进去。

简化口号:**codegen ≡ "把 Atlas.Client 的 C# entity 用 C++ 复刻"**。

## UE 客户端策略

走 **registry-driven decode + 类型化薄包装**:

- decode/dispatch:`ClientEntity::ApplyDelta` 通用一份,通过 `EntityDefRegistry` 查表逐字段 decode,所有 entity 共用
- 对外 API 类型化:codegen 生成 `Avatar.gen.h` 提供 `e->Hp()` / `e->ReportPos(pos, dir)` 等类型安全接口,内部仍走 registry

## CppEmitter 工具

`src/csharp/Atlas.Tools.CppEmitter/`,与 `Atlas.Tools.DefDump` 同级:

- C# 写,`dotnet run` 调用
- 直接读 `.def` XML,复用从 `Atlas.Generators.Def` 抽出的 `Atlas.Defs.Parser` lib
- 输出到 `samples/mvp/UEClient/Source/UEClient/gen/`(游戏模块本地,gitignored;跟 `Atlas.Mvp.Client` 在 C# 那侧对称)
- 由 `tools/bin/build_mvp_ue.bat` 在 UBT 之前自动调用;手工运行用 `tools/bin/cpp_emit.bat`

为什么用 C#:`DefParser` / `DefTypeExprParser` / `DefLinker` 已在 C# 侧成熟(2000+ 行),Python 重写=双份维护。

为什么不在 `Atlas.Generators.Def` 里 emit C++:Source Generator 设计上 emit `.cs`,emit `.h/.cpp` 是反模式;且 Source Generator 跑在 C# 编译期,emit C++ 反而把 C# 编译路径绑到 UE 构建上。独立工具更干净。

## 输出物

每个 entity 一份 `<EntityName>.gen.h`(header-only),纯 C++,继承 `atlas::ClientEntity`:

| 内容 | 状态 | 来源于 .def |
|------|------|-------------|
| 类型化标量 getter(`e->Hp() -> int32_t`) | M1 已落地 | `<properties>` |
| `ClientEntity::ApplyDelta`(通用,基类) | M1 已落地(registry-driven,scalar+container+struct) | base 类 + `EntityDefRegistry` |
| 上行 RPC stub(`e->ReportPos(pos, dir)`,SpanWriter 打包 + `SendCellRpc`) | M1 已落地 | `<cell_methods>` / `<base_methods>`(本 entity 可调方向) |
| 属性变化虚 hook(`virtual void OnHpChanged(int old, int neu)`) | M2 | `<properties>` |
| 下行 RPC 虚处理(`virtual void ShowDamage(int amount, uint32 attacker)`) | M2 | `<client_methods>` |
| `DispatchRpc(rpc_id, trace_id, reader)` override(派发到虚方法) | M2 | `<client_methods>` |
| `<types>` struct getter / container getter | M2(解码已在 base,getter 待 codegen 跟进) | `<types>` |
| `<components>` → 纯 C++ logic component 类 | M2 | `<components>` |

游戏侧使用方式:

```cpp
// 上行 RPC 已可用(M1)
account->SelectAvatar(1);                   // 走 SendBaseRpc
avatar->ReportPos(pos, dir);                // 走 SendCellRpc

// 属性 getter 已可用(M1)
int32_t hp = avatar->Hp();                  // registry-driven decode 之后读

// M2 之后:
class GameAvatar : public atlas::mvp::Avatar {
  void OnHpChanged(int /*old*/, int neu) override { /* hit flash, HUD update */ }
  void ShowDamage(int amount, uint32_t attacker) override { /* damage number */ }
};
```

## 几个 codegen 决策

| 决策 | 取值 | 备注 |
|------|------|------|
| Position 字段 | 走 volatile envelope `0xF001`,不参与 `ApplyReplicatedDelta` | 沿用 `ATLAS_DEF008`,与 C# 端一致 |
| 命名空间 | `atlas::mvp::Avatar`(对应 C# `Atlas.Mvp.Client.Avatar`) | 多 game project 时各自起 namespace |
| RPC reply(`trace_id + reply_bit`) | 生成 stub 但不实现 request-response 同步等待 | 真有 async-RPC 需求时再做 |
| 输出位置 | `samples/mvp/UEClient/Source/UEClient/gen/` | 游戏模块本地;不是 plugin 的事 |

## 构建链路

```
.def + entity_ids.xml
     │
     ▼
[CppEmitter]  (dotnet run; Atlas.Defs.Parser 解析)
     │
     ▼
samples/mvp/UEClient/Source/UEClient/gen/*.gen.h
     │
     ▼
[UBT]  ←— 链接 atlas_net_client.dll + atlas_entitydef_client.dll (CMake 产出)
                ↑
                └─ entity_defs.bin (DefDump 从 Atlas.Mvp.Client.dll 提取)
```

入口脚本 `tools/bin/build_mvp_ue.bat` 一条命令跑完:CMake → DefDump → CppEmitter → stage → UBT。每段都有 `--skip-*` 跳过。
