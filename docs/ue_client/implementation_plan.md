# 分阶段实施

> 本文档随实施推进**逐 milestone 删减**。完成的章节移除,仅保留架构/决策文档作为长期资料。

## 范围

只覆盖 UE 客户端 SDK + codegen + MVP 集成。不覆盖:D2 共享移动仿真、战斗 kernel 下沉、5000 并发压测、Iris/GAS/Mass。详见 [open_questions.md](open_questions.md)。

## 总览

| 阶段 | 工时 | 累计 | 状态 |
|------|------|------|------|
| ~~M0 — 端到端贯通~~ | ~~1 周~~ | — | **已完成**(详见 `samples/mvp/README.md` 的 UE client 章节) |
| ~~M1 — Codegen 上线 + 属性同步~~ | ~~1.5 周~~ | — | **已完成**(`Atlas.Defs.Parser` lib + `Atlas.Tools.CppEmitter` + 通用 `ClientEntity::ApplyDelta` + 端到端 build pipeline) |
| ~~M2 — 下行 RPC + BP 暴露 + Logic Component~~ | ~~1 周~~ | — | **已完成**(codegen 下行 dispatch + 属性变化虚 hook、`UAtlasAvatarView` BP delegate、Logic Component server+client 全链路) |
| M3 — MVP 功能对齐 | 1 周 | 1 | 待启动 |

单人投入。M2 全部交付已落地;M3 焦点在 UE 客户端追平 Unity 端 demo(输入接 RPC、HUD、子弹/技能演示)。

## M3 — MVP 功能对齐 + 工具链

**目标**:UE 客户端达到 `samples/mvp/UnityClient` 等价功能。

**交付**
- 登录界面 + HUD + 子弹/技能演示
- 客户端权威移动(WASD → `Avatar.ReportPos`)
- `samples/mvp/README.md` + `readme_cn.md` 双语更新
- Unity + UE 双客户端共存测试

**验收**:两客户端同 server,所有 demo 操作行为一致。

## 推进规则

- 每完成一个 milestone,**先清理本目录的过时内容**:删除已落地的设计描述,实施完毕的 milestone 章节从本文件移除
- 完成 M3 后整份 `implementation_plan.md` 可删,仅保留 architecture / decisions / codegen / open_questions
- 用户工作流变更(MVP 工具/行为)走中英双语 README 同步
- 按项目规则,每个 commit 配 patch(`./tmp/patches/`,顺序编号)
