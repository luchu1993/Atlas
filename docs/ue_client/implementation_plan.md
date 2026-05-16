# 分阶段实施

> 本文档随实施推进**逐 milestone 删减**。完成的章节移除,仅保留架构/决策文档作为长期资料。

## 范围

只覆盖 UE 客户端 SDK + codegen + MVP 集成。不覆盖:D2 共享移动仿真、战斗 kernel 下沉、5000 并发压测、Iris/GAS/Mass。详见 [open_questions.md](open_questions.md)。

## 总览

| 阶段 | 工时 | 累计 | 状态 |
|------|------|------|------|
| ~~M0 — 端到端贯通~~ | ~~1 周~~ | — | **已完成**(详见 `samples/mvp/README.md` 的 UE client 章节) |
| M1 — Codegen 上线 + 属性同步 | 1.5 周 | 1.5 | 待启动 |
| M2 — 双向 RPC + Logic Component | 1 周 | 2.5 | 待启动 |
| M3 — MVP 功能对齐 | 1 周 | 3.5 | 待启动 |

单人投入。M1 的 CppEmitter 和 C++ `FClientEntityBase` 可拆开并行,可压到约 3 周。

## M1 — Codegen 上线 + 属性同步

**目标**:扔掉 M0 手写 Stub,所有 entity 由 codegen 产出;属性同步覆盖全字段类型。

**交付**
- 从 `Atlas.Generators.Def` 抽出 `Atlas.Defs.Parser` lib(Source Generator 与新工具共享)
- `Atlas.Tools.CppEmitter` 工具
- `FClientEntityBase::ApplyDelta` 通用实现(registry-driven,复用 `EntityDefRegistry`)
- CppEmitter 输出:entity + `<types>` struct + logic `<components>` 纯 C++ 类
- `UAtlasAvatarView` UCLASS + view bridge
- Build 链路:`tools/bin/build_mvp_ue.bat` 一次命令完成

**验收**
- `entity_defs/Avatar.def` 全字段在 UE 端正确同步(含 list/dict/struct)
- 单元测试:标量 / struct / list[T] / dict[K,V] byte-for-byte 对照 Unity 端 wire
- BP 中 `GetHp` / 绑 `OnHpChanged` 工作
- 现有 C# Source Generator 测试不退化

**化解风险**:codegen 工具链跨平台 / UHT 兼容 / logic component slot 一致性

## M2 — 双向 RPC + Logic Component

**目标**:打通 client↔server RPC,logic component 字段/RPC 平铺暴露到 BP。

**交付**
- CppEmitter:`client_methods` → `I<Entity>View::On*` typed 虚方法
- CppEmitter:exposed `cell_methods` / `base_methods` → BlueprintCallable 上行 stub
- Logic component 字段/RPC 透传到 `UAtlasAvatarView` 平铺接口
- RPC 安全校验对齐 server `OnClientCellRpc` 链(self vs other-entity,exposed scope)

**验收**
- 闭环:UE 输入 → `CastSkill` → server tick → `OnSkillHit` → UE BP 特效
- 行为与 Unity 端肉眼一致(录屏对比)
- 越权 RPC(跨 entity / 非 exposed)被 server 拒绝

## M3 — MVP 功能对齐 + 工具链

**目标**:UE 客户端达到 `samples/mvp/UnityClient` 等价功能。

**交付**
- `samples/mvp/UEClient/`(与 `UnityClient/` 平级)
- 登录界面 + HUD + 子弹/技能演示
- `tools/setup_mvp_ue.py` + `.bat`(类比 `setup_mvp_unity.py`)
- `tools/build_mvp_ue.py` + `.bat`
- `samples/mvp/README.md` + `readme_cn.md` 双语更新
- Unity + UE 双客户端共存测试

**验收**:两客户端同 server,所有 demo 操作行为一致。

## 推进规则

- 每完成一个 milestone,**先清理本目录的过时内容**:删除已落地的设计描述,实施完毕的 milestone 章节从本文件移除
- 完成 M3 后整份 `implementation_plan.md` 可删,仅保留 architecture / decisions / codegen / open_questions
- 用户工作流变更(MVP 工具/行为)走中英双语 README 同步
- 按项目规则,每个 commit 配 patch(`./tmp/patches/`,顺序编号)
