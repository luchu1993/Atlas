# UE 客户端接入

> 状态: 当前 UE MVP 接入索引。UE 已接入登录、HUD、SpaceData、Avatar view、
> 输入帧、owner predictor、movement ack / command 和 generated RPC；剩余表现层缺口见 MVP README。
>
把 Unreal Engine 作为 Atlas 的纯渲染前端。**不使用** UE 的 Replication / NetDriver / UFUNCTION(Server,Client) / GAS / Iris / Mass。

Atlas 服务端通过自己的 wire 协议(`0xF001`–`0xF007` over RUDP)向 UE 客户端推送
entity 状态、RPC、movement ack 和 MovementCommand,UE 端通过 C++ Plugin 接入,
不寄宿 CoreCLR。

## 索引

- [architecture.md](architecture.md) — Entity-owns-View 模型、三层分层、UE 集成边界
- [decisions.md](decisions.md) — 已锁的关键决策清单
- [codegen.md](codegen.md) — `Atlas.Tools.CppEmitter` 工具与生成产物
- [implementation_plan.md](implementation_plan.md) — 当前 MVP 对齐状态和剩余表现层缺口
- [open_questions.md](open_questions.md) — 延期事项与后续决策点

## 关键约束

- **UE 5.x source build**
- 路径 B(C++ 重写客户端 SDK,不寄宿 CoreCLR)
- Plugin 位置:`samples/mvp/UEClient/Plugins/AtlasUE/`
- 复用 `atlas_net_client.dll`(C++ 网络栈,CMake 产出)
- 当前完整 UE Editor 本地 loop 是 Windows / Win64；`.sh` wrapper 保留脚本对称性，
  但 sample plugin 的 ThirdParty 链接与运行时 DLL 加载仍只接了 Win64。
- 当前能力:Unity 与 UE 同 server 同房间互可见;UE 已接入登录、HUD、
  SpaceData、Avatar view、WASD/Space 输入帧、owner predictor、movement
  ack / command 和 generated RPC。剩余表现层缺口见
  [`samples/mvp/UEClient/README.md`](../../samples/mvp/UEClient/README.md)。
