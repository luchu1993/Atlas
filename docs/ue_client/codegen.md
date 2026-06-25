# Codegen 设计

> 状态: 当前 UE C++ codegen 设计。`Atlas.Tools.CppEmitter` 与
> `tools/bin/cpp_emit.{bat,sh}` 已接入；Blueprint 表面仍由游戏侧 view 层手写。
>
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
- 对外 API 类型化:codegen 生成 `Avatar.gen.h`，提供 `e->Hp()` /
  `e->LaunchProjectile(dir)` 等类型安全接口，内部仍走 registry

## CppEmitter 工具

`src/csharp/Atlas.Tools.CppEmitter/`,与 `Atlas.Tools.DefDump` 同级:

- C# 写,`dotnet run` 调用
- 直接读 `.def` XML,复用从 `Atlas.Generators.Def` 抽出的 `Atlas.Defs.Parser` lib
- 输出到 `samples/mvp/UEClient/Source/UEClient/gen/`(游戏模块本地,gitignored;跟 `Atlas.Mvp.Client` 在 C# 那侧对称)
- 由 `tools/bin/build_mvp_ue.{bat,sh}` 在 UBT 之前自动调用;手工运行用
  `tools/bin/cpp_emit.{bat,sh}`

为什么用 C#:`DefParser` / `DefTypeExprParser` / `DefLinker` 已在 C# 侧成熟(2000+ 行),Python 重写=双份维护。

为什么不在 `Atlas.Generators.Def` 里 emit C++:Source Generator 设计上 emit `.cs`,emit `.h/.cpp` 是反模式;且 Source Generator 跑在 C# 编译期,emit C++ 反而把 C# 编译路径绑到 UE 构建上。独立工具更干净。

## 输出物

每个 entity 一份 `<EntityName>.gen.h`(header-only),纯 C++,继承 `atlas::ClientEntity`。每个 synced 标准 logic component 同步产出 `<ComponentName>.gen.h`,继承 `atlas::ComponentInstance`;entity 头自动 `#include` 自己挂载的 component 头并在构造里注册 slot factory。所有 entity / component 共享一份 `_Structs.gen.h`(linker 收集到的全部 `<types><struct>` 声明 + `Serialize/Deserialize/FromStructValue`)避免跨文件循环 include。

| 内容 | 来源于 .def |
|------|-------------|
| 类型化标量 getter(`e->Hp() -> int32_t`) | `<properties>` |
| 容器 / struct getter(运行时 variant → 类型化 vector / POD) | `<properties>` 中容器或 struct 类型 |
| `ClientEntity::ApplyDelta`(registry-driven 通用基类,scalar+container+struct+component section) | `EntityDefRegistry` |
| 属性变化虚 hook(`virtual void OnHpChanged(int old, int neu)`)+ `ApplyDelta` override snapshot + diff fire | `<properties>` |
| 上行 RPC stub（`e->LaunchProjectile(dir)`，SpanWriter 打包 + `SendCellRpc` / `SendBaseRpc`） | `<cell_methods>` / `<base_methods>` |
| 下行 RPC 虚处理(`virtual void ShowDamage(int amount, uint32 attacker)`) | `<client_methods>` |
| `DispatchEntityRpc(rpc_id, trace_id, reader)` override(派发到虚方法) | `<client_methods>` |
| Component slot accessor(`avatar->load() -> StressLoadComponent*`)+ `RegisterComponentFactory(slot, ...)` 在 entity 构造体里自动调用 | `<components>` 中 synced 项 |
| Component 端的属性 getter、change hook、上行 RPC stub(routes via owner→Sender,rpc_id 自动塞入 slot_idx)、下行 `DispatchRpc(method_idx)` switch | `<component>` 文件 |

游戏侧使用方式:

```cpp
// 上行 RPC
account->SelectAvatar(1);                   // 走 SendBaseRpc
avatar->LaunchProjectile(dir);              // 走 SendCellRpc

// 属性 getter
int32_t hp = avatar->Hp();                  // registry-driven decode 之后读

// 下行 hook(scalar 属性 + client_methods)
class GameAvatar : public atlas::mvp::Avatar {
  void OnHpChanged(int /*old*/, int neu) override { /* hit flash, HUD update */ }
  void ShowDamage(int amount, uint32_t attacker) override { /* damage number */ }
};

// Logic component:slot accessor + override 同样的 hook 模式
auto* load = avatar->load();                // slot 1 → StressLoadComponent
int32_t extra = load->ExtraHp();
load->Charge(seq, amount);                  // 上行 cell RPC,rpc_id 自动带 slot
```

BP 暴露走游戏侧手写 view(参考 `samples/mvp/UEClient/Source/UEClient/AtlasAvatarView.h`):
游戏侧的 `FBpAvatarEntity : atlas::mvp::Avatar` override 上述 hook,把参数转 UE 类型(`atlas::Vec3` → `FVector` via `AtlasToUE`、`uint32` → `int32` 满足 dynamic delegate 反射约束)后 `Broadcast` 到 `UAtlasAvatarView` 的 `BlueprintAssignable` delegate。**Logic component 自己不感知 view**,需要广播到 BP 的事件统一在 entity view 上聚合。

## 几个 codegen 决策

| 决策 | 取值 | 备注 |
|------|------|------|
| Position 字段 | 走 volatile envelope `0xF001`,不参与 `ClientEntity::ApplyDelta` | 沿用 `ATLAS_DEF008`,与 C# 端一致 |
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

入口脚本 `tools/bin/build_mvp_ue.{bat,sh}` 一条命令跑完:CMake → DefDump →
CppEmitter → stage → UBT。当前 sample plugin 的 ThirdParty 链接与运行时 DLL
加载只接 Win64；非 Win64 UE build 需要先扩展 `AtlasUE.Build.cs` 和
`AtlasUE.cpp`。分段跳过用 `--skip-native` / `--skip-defs` /
`--skip-codegen` / `--skip-stage` / `--skip-ue`。
