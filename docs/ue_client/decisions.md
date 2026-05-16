# 已锁决策

## D1 — 客户端 SDK 路径

**选 B**:C++ 重写 `Atlas.Client` 等价物,不在 UE 寄宿 CoreCLR。

理由:UE Shipping 带 CoreCLR 长期是债(~30MB,AOT 集成空白);Unity 已独占 C# 路径,UE 走 C++ 让 codegen 输出对称(server C++ + UE C++ + Unity C#);调试链路单语言更简单。

## D2 — 共享移动仿真

**延期**。详见 [open_questions.md](open_questions.md)。

## D3 — UE 版本

**5.7**

## D4 — Entity ↔ Actor 关系

Entity 拥有 View,View 持有 Actor 弱引用。Actor 是 Entity 的渲染句柄,不持有 Atlas 状态。详见 [architecture.md](architecture.md)。

## D5 — Codegen 工具语言

**C#**。工具名 `Atlas.Tools.CppEmitter`,放 `src/csharp/`。`tools/bin/cpp_emit.bat` / `.sh` 提供薄包装。

理由:`DefParser` / `DefTypeExprParser` / `DefLinker` 已在 C# 侧成熟(2000+ 行)。Python 重写等于双份维护协议解析。**def-related codegen 是 `CLAUDE.md` Python 偏好的显式例外**,本目录是唯一明文记录处。

## D6 — `.def <component>` 与 `UActorComponent` 的关系

完全独立,不映射。Logic component 字段/RPC 平铺到 `UAtlasAvatarView`,**不**为每个 logic component 生成独立 UActorComponent。详见 [architecture.md](architecture.md)。

## D7 — Codegen decode 范式

**Registry-driven decode + 类型化 accessor**。

- `FClientEntityBase::ApplyDelta` 通用一份,通过 `EntityDefRegistry` 查表逐字段 decode
- 但 codegen 生成类型化 `Avatar.gen.h`,对外是 `E->Health()` 风格 API

理由:`EntityDefRegistry`(ATDF)已是 C++ 端唯一元数据来源,decode 逻辑只维护一份;新增 entity 类型不需要 C++ 重编。

副作用:服务端新加 entity 类型时,UE 端如果只接收(不要 BP typed 暴露)零成本;要做 typed UCLASS 暴露才跑一次 codegen。

## D8 — Plugin 位置与 DLL 复用

- Plugin:`samples/mvp/UEClient/Plugins/AtlasUE/`
- Native DLL:沿用 `atlas_net_client.dll`(CMake 产出,UE Plugin 通过 ThirdParty 引用)

## D9 — UE 自带系统取舍

不用:NetDriver / Replication / `UFUNCTION(Server,Client,NetMulticast)` / GAS / Iris / Mass / `CharacterMovementComponent` 的网络部分。

可用:Enhanced Input(转发到 Atlas RPC)、Blueprint / UMG、动画系统(以 Atlas 字段为驱动)。

## D10 — 战斗代码共享

**本期不实现**(UE 端只插值不预测)。M3 完成后视实测决策路径 A 或 B+下沉。详见 [open_questions.md](open_questions.md)。

## D11 — Port parity 约束

从 `Atlas.Client` C# 移植到 UE C++ 的逻辑(AvatarFilter、envelope decoder、ClientEntityManager 等)**不得修改 C# 端源码**,且必须在每次 port 完成时验证:

- `dotnet test tests/csharp` 全部通过
- Unity 客户端登录 + 基本同步行为不退化

C# 端是单一来源,C++ port 是"只读对照实现",任何行为差异(浮点、wire 字段顺序、状态机)以 C# 为准修改 C++。

阻塞情况(C# 端真有 bug 或语义模糊):停下来讨论,不在 UE 侧打补丁绕过。
