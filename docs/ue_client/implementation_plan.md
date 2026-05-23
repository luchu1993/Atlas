# UE MVP 对齐状态

> 本文档只保留当前未完全收敛的 UE MVP 对齐状态。已完成的 M0-M2 细节以
> `samples/mvp/UEClient/README.md` 和代码为准。

## 范围

只覆盖 UE 客户端 SDK + codegen + MVP 集成。不覆盖:D2 共享移动仿真、战斗 kernel 下沉、5000 并发压测、Iris/GAS/Mass。详见 [open_questions.md](open_questions.md)。

## 总览

| 阶段 | 工时 | 累计 | 状态 |
|------|------|------|------|
| ~~M0 — 端到端贯通~~ | ~~1 周~~ | — | **已完成**(详见 `samples/mvp/README.md` 的 UE client 章节) |
| ~~M1 — Codegen 上线 + 属性同步~~ | ~~1.5 周~~ | — | **已完成**(`Atlas.Defs.Parser` lib + `Atlas.Tools.CppEmitter` + 通用 `ClientEntity::ApplyDelta` + 端到端 build pipeline) |
| ~~M2 — 下行 RPC + BP 暴露 + Logic Component~~ | ~~1 周~~ | — | **已完成**(codegen 下行 dispatch + 属性变化虚 hook、`UAtlasAvatarView` BP delegate、Logic Component server+client 全链路) |
| M3 — MVP 功能对齐 | 1 周 | 1 | 部分完成 |

单人投入。M2 全部交付已落地;M3 已接入登录、HUD 基类、SpaceData、Avatar
view、WASD/Space 输入、generated RPC 与本地 build/launch wrapper。剩余差距以
`samples/mvp/UEClient/README.md` 的 Known gaps 为准。

## M3 — MVP 功能对齐 + 工具链

**目标**:UE 客户端达到 `samples/mvp/UnityClient` 等价功能。

**已交付**
- 登录 / 自动登录路径、HUD C++ base、SpaceData/NPC count、Avatar view
- 客户端权威移动(WASD → `Avatar.ReportPos`)与 Space 触发 RPC
- `tools/run_mvp_ue.py` / `tools/build_mvp_ue.py` 本地 dev loop
- Unity + UE 双客户端同服互可见路径

**当前缺口**
- 输入 remapping / settings menu
- chat 输入与 scrollback
- equipment / weapon swap UI
- damage floater 3D actor 与 projectile trail VFX
- UE bot mode

## 推进规则

- 用户工作流变更(MVP 工具/行为)走中英双语 README 同步
- 按项目规则,每个 commit 配 patch(`./tmp/patches/`,顺序编号)
