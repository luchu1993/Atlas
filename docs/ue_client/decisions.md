# 已锁决策

> 状态: 当前 UE MVP 决策记录。这里记录仍然有效的选择；剩余问题集中在
> [open_questions.md](open_questions.md)。
>
## D1 — 客户端 SDK 路径

**选 B**:C++ 重写 `Atlas.Client` 等价物,不在 UE 寄宿 CoreCLR。

理由:UE Shipping 带 CoreCLR 长期是债(~30MB,AOT 集成空白);Unity 已独占 C# 路径,UE 走 C++ 让 codegen 输出对称(server C++ + UE C++ + Unity C#);调试链路单语言更简单。

## D2 — 共享移动仿真 / owner 预测

**已接入当前 MVP 路径**。UE 通过 `atlas_net_client` 复用 `movement_sim`
predict step,C++ 侧 `FOwnerMovementPredictor` 处理输入历史、ack replay、
visual offset 和 MovementCommand start / end。

## D3 — UE 版本

**UE 5.x source build, not pinned to a minor version.** `UEClient.uproject`
records the local source-build association, and wrappers resolve the active
engine from `UE_ROOT` or `--ue-root`.

## D4 — Entity ↔ Actor 关系

Entity 拥有 View,View 持有 Actor 弱引用。Actor 是 Entity 的渲染句柄,不持有 Atlas 状态。详见 [architecture.md](architecture.md)。

## D5 — Codegen 工具语言

**C#**。工具名 `Atlas.Tools.CppEmitter`,放 `src/csharp/`。`tools/bin/cpp_emit.bat` / `.sh` 提供薄包装。

理由:`DefParser` / `DefTypeExprParser` / `DefLinker` 已在 C# 侧成熟(2000+ 行)。Python 重写等于双份维护协议解析。**def-related codegen 是 `CLAUDE.md` Python 偏好的显式例外**,本目录是唯一明文记录处。

## D6 — `.def <component>` 与 `UActorComponent` 的关系

完全独立,不映射。Logic component 字段/RPC 平铺到 `UAtlasAvatarView`,**不**为每个 logic component 生成独立 UActorComponent。详见 [architecture.md](architecture.md)。

## D7 — Codegen decode 范式

**Registry-driven decode + 类型化 accessor**。

- `ClientEntity::ApplyDelta` 通用一份,通过 `EntityDefRegistry` 查表逐字段 decode
- 但 codegen 生成类型化 `Avatar.gen.h`,对外是 `E->Hp()` 风格 API

理由:`EntityDefRegistry`(ATDF)已是 C++ 端唯一元数据来源,decode 逻辑只维护一份;新增 entity 类型不需要 C++ 重编。

副作用:服务端新加 entity 类型时,UE 端如果只接收(不要 BP typed 暴露)零成本;要做 typed UCLASS 暴露才跑一次 codegen。

## D8 — Plugin 位置与 DLL 复用

- Plugin:`samples/mvp/UEClient/Plugins/AtlasUE/`
- Native DLL:沿用 `atlas_net_client.dll`(CMake 产出,UE Plugin 通过 ThirdParty 引用)

## D9 — UE 自带系统取舍

不用:NetDriver / Replication / `UFUNCTION(Server,Client,NetMulticast)` / GAS / Iris / Mass / `CharacterMovementComponent` 的网络部分。

可用:Enhanced Input(转成 Atlas movement input frame 或低频 RPC)、Blueprint / UMG、
动画系统(以 Atlas 字段为驱动)。

## D10 — 战斗代码共享

**本期不实现战斗预测共享**。UE 已接入移动预测；战斗数值 kernel 是否下沉或
寄宿仍等当前 UE MVP 表现层缺口收敛后,再按实测手感决策路径 A 或 B+下沉。
详见 [open_questions.md](open_questions.md)。

## D11 — Port parity 约束

从 `Atlas.Client` C# 移植到 UE C++ 的逻辑(AvatarFilter、envelope decoder、ClientEntityManager 等)**不得修改 C# 端源码**,且必须在每次 port 完成时验证:

- `dotnet test tests/csharp/Atlas.Runtime.Tests/Atlas.Runtime.Tests.csproj --configuration Debug` 通过
- `dotnet test tests/csharp/Atlas.Generators.Tests/Atlas.Generators.Tests.csproj --configuration Debug` 通过
- `dotnet test tests/csharp/Atlas.Client.Tests/Atlas.Client.Tests.csproj --configuration Debug` 通过
- Unity 客户端登录 + 基本同步行为不退化

C# 端是单一来源,C++ port 是"只读对照实现",任何行为差异(浮点、wire 字段顺序、状态机)以 C# 为准修改 C++。

阻塞情况(C# 端真有 bug 或语义模糊):停下来讨论,不在 UE 侧打补丁绕过。

## D12 — Layer 1 严格项目风格,Layer 2/3 走 UE 惯例

`AtlasCore/`(Layer 1,引擎无关 C++)严格遵循仓库项目风格:

- 2-space 缩进、attached braces、`namespace atlas::`
- PascalCase 函数、snake_case 变量、trailing `_` 成员
- 多类别可恢复错误用 `Result<T,E>` 或具名 `enum class`(参照 `EnvelopeDecodeResult`)

`AtlasUE` plugin + UEClient 游戏模块(Layer 2/3)走 UE 惯例:

- tab 缩进、Allman braces、PascalCase 标识符、`F/U/A/E` 类型前缀
- `UPROPERTY/UFUNCTION` 反射宏
- 简单失败用 `bool` 返回 + `UE_LOG` 记诊断;`Result<T,E>` 只在错误模式真有多分支需要分别处理时引入

**理由**:Layer 1 要保持可移植(未来 Unity-C++ port、独立 CLI 测试),严格项目风格;Layer 2/3 与 UE 生成代码、社区示例共存,偏离 UE 惯例只换来摩擦,不换回可移植性。

边界以目录划分:`Plugins/AtlasUE/Source/AtlasUE/Public/AtlasCore/` 和
`Plugins/AtlasUE/Source/AtlasUE/Private/AtlasCore/` 是 Layer 1,其余 UE 风格。
