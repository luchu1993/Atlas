# UE MVP 状态

> 状态: 当前 UE MVP 对齐状态。本文档只记录当前 UE MVP 状态和剩余表现层缺口。
> 已完成的接入细节以
> `samples/mvp/UEClient/README.md` 和代码为准。

## 范围

只覆盖 UE 客户端 SDK + codegen + MVP 集成。不覆盖:战斗 kernel 下沉、
5000 并发压测、Iris/GAS/Mass。详见
[open_questions.md](open_questions.md)。

## 总览

- 登录 / 自动登录路径、HUD C++ base、SpaceData/NPC count、Avatar view
- 服务端权威 owner movement（WASD → input frame → native predictor / ack replay）
  与 MovementCommand start / end 解码
- generated RPC、属性 delta / component decode 与 BP view bridge
- `tools/bin/run_mvp_ue.{bat,sh}` / `tools/bin/build_mvp_ue.{bat,sh}` 本地 dev loop；
  当前完整 UE plugin 链接 / Editor 运行路径只验证到 Windows / Win64
- Unity + UE 双客户端同服互可见路径

## 当前缺口

以 `samples/mvp/UEClient/README.md` 的 Known gaps 为准：

- 输入 remapping / settings menu
- chat 输入与 scrollback
- equipment / weapon swap UI
- damage floater 3D actor 与 projectile trail VFX
- UE bot mode
- 非 Win64 UE plugin ThirdParty 链接与运行时加载尚未接入
- 录制/回放工具链尚未覆盖双客户端；MovementCommand playback 已接入 Unity / UE 移动路径
