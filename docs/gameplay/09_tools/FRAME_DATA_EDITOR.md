# 帧数据编辑器（Frame Data Editor）

> **用途**：定义 Atlas 的 Unity Editor 内嵌工具——让策划可视化地查看与调整技能时间轴、命中窗口、动画对齐、效果触发时机。这是产能的核心工具，没有它策划无法高效产出 1500+ 技能。
>
> **读者**：工具开发（必读）、战斗策划（必读）、动画师（§5、§7 必读）、Tech Lead（必读）。
>
> **状态**：草案 v0.1 — 待团队评审。
>
> **前置文档**：`OVERVIEW.md`、`SKILL_SYSTEM.md`、`COMBAT_FEEL.md`、`DATA_PIPELINE.md`
>
> **关联文档**：`HIT_VALIDATION.md`（hitbox 可视化）、`COMBAT_ACTIONS.md`（action 类型）

---

## 1. 设计目标与边界

### 1.1 为什么必需

按 OVERVIEW 项目规模（25 职业 × 60 技能 = 1500 技能）+ 团队结构（最终 100+ 人，美术 > 策划 >> 技术），**工具产能决定项目产能**。

如果策划只能：
- 在 Excel 改 `time_ms = 150`
- 启动游戏跑一次看效果
- 不对再改 Excel
- 反复

每个技能调整一轮 = 5 分钟。1500 技能 × 平均 30 轮迭代 = **3750 工时**——单凭这就是 2 人年。

帧数据编辑器目标：
- **改 → 看 ≤ 3 秒**（Editor 内实时预览）
- **关键参数 GUI 调节**（不切回 Excel）
- **可视化时间轴**（Gantt 图 + 帧标尺）
- **预计将每技能调整时间降到 30 秒**

### 1.2 设计目标

1. **可视化优先**：时间轴 Gantt 图 + 实时预览角色动画 + hitbox 形状
2. **双向数据流**：编辑器改动 → 同步回 Excel（或中间格式）
3. **零启动成本**：在 Unity Editor 内开窗即用，不切外部工具
4. **批量编辑**：可同时打开多个技能对照
5. **支持 P3 验证**：录制对比、参数 A/B、回归测试

### 1.3 非目标

- **不替代 Excel**：Excel 仍是真源，编辑器是 GUI 包装
- **不做完整动画系统**：不动 Animator state machine 内部，仅读取
- **不在线编辑**：仅 Editor 内（不发布给玩家）
- **不做行为树编辑**：AI 用单独工具

### 1.4 与 DATA_PIPELINE 的关系

```
策划在 Excel 编辑 ⟷ Frame Data Editor (双向同步)
                       ↓
                  保存到 Excel
                       ↓
              DataBuild → .bytes
                       ↓
              运行时（服务端 + 客户端）
```

编辑器**不绕过** DATA_PIPELINE——所有编辑最终回写 Excel。

---

## 2. 总体架构

### 2.1 模块组成

```
┌──────────────────────────────────────────────────────┐
│  Unity Editor 内嵌窗口                                │
│  ┌────────────────────────────────────────────────┐  │
│  │ Top Toolbar                                    │  │
│  │ [Open Skill] [Save] [Run Preview] [Step Frame] │  │
│  └────────────────────────────────────────────────┘  │
│  ┌─────────────────┐  ┌────────────────────────────┐ │
│  │                 │  │                            │ │
│  │  Skill List     │  │  Timeline View (Gantt)     │ │
│  │  (Tree)         │  │  ─ 时间标尺                 │ │
│  │  ├ Warrior      │  │  ─ 多轨道（Anim/Hitbox/    │ │
│  │  │ ├ slash      │  │     VFX/Sound/Camera...）   │ │
│  │  │ └ combo      │  │  ─ 事件块（拖拽编辑）        │ │
│  │  └ Archer       │  │  ─ 当前播放头              │ │
│  │                 │  └────────────────────────────┘ │
│  │                 │  ┌────────────────────────────┐ │
│  │                 │  │  Inspector                 │ │
│  │                 │  │  当前选中事件的参数         │ │
│  │                 │  │  - action_type             │ │
│  │                 │  │  - time_ms                 │ │
│  │                 │  │  - 各 action-specific 字段 │ │
│  │                 │  └────────────────────────────┘ │
│  └─────────────────┘                                  │
│  ┌────────────────────────────────────────────────┐  │
│  │  Preview Pane                                   │  │
│  │  - Unity Scene 内角色 model + Animator           │  │
│  │  - Hitbox 形状 wireframe 实时显示                │  │
│  │  - VFX 预览                                     │  │
│  │  - 受击假人对照位置                              │  │
│  └────────────────────────────────────────────────┘  │
└──────────────────────────────────────────────────────┘
```

### 2.2 实现技术栈

| 组件 | 实现 |
|---|---|
| 窗口框架 | `EditorWindow` + IMGUI（v1） / UI Toolkit（v2） |
| 时间轴 | 自定义 IMGUI 绘制 + 鼠标交互 |
| 预览 | Scene View 复用 + 自定义渲染 |
| 数据读取 | 读 `data/generated/*.bytes`（FlatBuffers） |
| 数据保存 | 写 Excel via EPPlus（与 DataBuild 共用） |
| 动画播放 | Unity Animator 主接口 + `Animator.Update(dt)` 驱动 |
| Hitbox 可视化 | `Handles.DrawWireDisc` / `Gizmos.DrawWireSphere` 等 |

### 2.3 项目位置

```
unity_project/Assets/Editor/AtlasFrameDataEditor/
├── Window/
│   ├── FrameDataEditorWindow.cs        # 主窗口
│   └── FrameDataEditorWindow.uxml      # v2 UI Toolkit
├── Timeline/
│   ├── TimelineGui.cs                  # Gantt 绘制
│   ├── TrackRenderer.cs                # 轨道渲染
│   └── EventBlockEditor.cs             # 事件块拖拽
├── Preview/
│   ├── PreviewScene.cs                 # 预览场景
│   ├── HitboxGizmos.cs                 # hitbox 可视化
│   └── PreviewRunner.cs                # 时间推进
├── DataAccess/
│   ├── SkillDataLoader.cs              # 读 bytes
│   ├── ExcelWriter.cs                  # 写 Excel
│   └── LiveSync.cs                     # 文件 watcher
└── Inspector/
    ├── EventInspector.cs               # 事件参数编辑
    └── ActionTypeRenderers.cs          # 每种 action 自定义 GUI
```

---

## 3. Timeline View（核心 UI）

### 3.1 视觉布局

```
              ┌──────────────────────────────────────────────┐
Time(ms):  0  │ 100   200   300   400   500   600   700      │
              ├──────────────────────────────────────────────┤
Animation     │ ████ slash_start ██▌                          │
              ├──────────────────────────────────────────────┤
Hitbox        │            ▌███ hb_cone_1 ████▌               │
              ├──────────────────────────────────────────────┤
Movement      │ ▌█ Dash forward █▌                            │
              ├──────────────────────────────────────────────┤
StateFlag     │   ▌ super_armor █████████████▌                │
              │                  ▌ cancel_window █▌           │
              ├──────────────────────────────────────────────┤
VFX           │            ▌ slash_trail ████▌                │
              ├──────────────────────────────────────────────┤
Sound         │ ▌whoosh▌            ▌impact▌                  │
              ├──────────────────────────────────────────────┤
HitPause      │              ▌HP 80ms▌                        │
              ├──────────────────────────────────────────────┤
CameraShake   │              ▌shake 200ms▌                    │
              └──────────────────────────────────────────────┘
                                   ▲
                                   │
                              当前播放头
```

### 3.2 轨道分类

按 Action Type 分轨道：
- **Animation**（PlayAnim / PlayAnimLoop / SetAnimParam）
- **Hitbox**（SpawnHitbox / RemoveHitbox），高亮其 active 期
- **Movement**（Dash / Launch / Pull / Knockback）
- **StateFlag**（SuperArmor / Iframe / CancelWindow）
- **Buff**（ApplyBuff / RemoveBuff）
- **Spawn**（SpawnProjectile / SpawnSubSkill）
- **VFX**（PlayVFX）
- **Sound**（PlaySound）
- **Camera**（HitPause / Shake / Slomo / Focus）
- **Damage**（DamageTarget / HealTarget），通常通过 Hitbox 间接

### 3.3 事件块交互

| 操作 | 行为 |
|---|---|
| 单击 | 选中，参数显示在 Inspector |
| 拖拽 | 调整 `time_ms`（按帧吸附 vs 自由） |
| 拖拽边缘 | 调整 duration（如 hitbox lifetime） |
| 双击 | 进入参数编辑模式 |
| 右键 | 菜单（删除 / 复制 / 改类型） |
| Shift+点击 | 多选 |
| Ctrl+D | 复制选中事件 |
| Delete | 删除选中事件 |
| 拖出窗口 | 删除（视觉反馈） |

### 3.4 时间标尺

- 横轴单位：毫秒（默认）
- 可切换：毫秒 / 帧（30 FPS） / 帧（60 FPS）
- 缩放：滚轮缩放，Ctrl+滚轮精细
- 平移：中键拖拽

支持帧数显示，方便动画师对齐：

```
Frame@30fps:  0     3     6     9     12     15     18     21
Time(ms):     0   100   200   300   400    500    600    700
```

### 3.5 标记线

特殊时间点：
- 当前播放头（绿色竖线）
- 总时长边界（红色竖线）
- 取消窗口起始/结束（黄色竖线）
- 命中关键帧（蓝色竖线，由动画师标注）

---

## 4. Inspector 参数编辑

### 4.1 通用字段

每个事件都有：
- `time_ms`：滑块 + 数字输入
- `action_type`：下拉框（重选会清空 specific 字段）
- 注释（不存数据库，仅工具内）

### 4.2 Action-specific 编辑

每种 ActionType 有定制化 GUI：

#### SpawnHitbox
```
hitbox_def_id:    [Dropdown 选 HitboxDef] [Edit 跳转编辑]
attach_bone_id:   [Dropdown 列骨骼名]
local_offset:     [vec3 输入]
lifetime_ms:      [Slider 0–2000]
damage_dsl_ref:   [Dropdown 列 DSL 片段] [Test 试算]
on_hit_dsl_ref:   [同上]
target_filter:    [Dropdown enum]
max_targets:      [Int]
```

预览面板实时显示 hitbox wireframe（按 def 形状）。

#### Dash
```
distance_m:       [Slider 0–20]
direction:        [vec3 / Dropdown(Forward/...)]
duration_ms:      [Slider 100–2000]
curve_id:         [Dropdown 列曲线] [Visualize 显示曲线图]
input_policy:     [Dropdown enum]
```

预览：在场景内画从当前位置到目标位置的曲线轨迹。

#### HitPause
```
target_scope:     [Dropdown: AttackerOnly/VictimOnly/Both]
time_scale:       [Slider 0.0–1.0]
duration_ms:      [Slider 30–200]
```

预览：触发 → 实时看到角色动画暂时减速。

#### CameraShake
```
intensity_m:      [Slider 0.05–0.5]
duration_ms:      [Slider 50–500]
frequency_hz:     [Slider 5–50]
falloff_curve:    [Dropdown / Curve Editor]
axis_weight:      [vec3 sliders]
```

预览：相机实际抖动。

### 4.3 公共字段验证

实时校验：
- `time_ms < skill.duration_ms` 且 ≥ 0
- 外键存在（如 hitbox_def_id 必须存在于 HitboxDefs）
- 数值范围（如 crit_chance ∈ [0, 1]）

违规字段红框 + 错误提示。

### 4.4 DSL 内联编辑

对于 `damage_dsl_ref` 等字段：
- 默认显示 dropdown 选已有 DSL
- 点击 "Edit Inline" → 内嵌文本编辑器
- 实时语法 + 类型检查
- 编辑完成后**自动写到 Dsl.xlsx**（新增片段）或**修改已有片段**

避免策划频繁切到外部编辑器。

---

## 5. Preview Pane（实时预览）

### 5.1 预览场景

Unity Scene 内的隔离场景：
- 一个 caster（玩家角色 prefab）
- 一个 victim（训练假人）
- 简单地面 + skybox
- 相机绑定到玩家相机配置

预览不影响主项目场景。

### 5.2 时间推进

```csharp
public sealed class PreviewRunner {
  SkillInstance _instance;
  long _previewTick = 0;
  bool _isPlaying = false;
  bool _stepMode = false;
  float _playSpeed = 1.0f;
  
  public void Play() { _isPlaying = true; }
  public void Pause() { _isPlaying = false; }
  public void StepForward() { 
    AdvanceOneTick(); 
  }
  public void Reset() { 
    _previewTick = 0; 
    RecreateInstance(); 
  }
  
  void Update() {
    if (!_isPlaying) return;
    _previewTick += Mathf.RoundToInt(_playSpeed);  // 按速度推进
    AdvanceOneTick();
    if (_previewTick >= _instance.Def.Timeline.DurationMs / TickMs) {
      Pause();  // 自然结束
    }
  }
  
  void AdvanceOneTick() {
    _instance.TimelineRunner.Tick(_ctx);
    UpdatePreviewVisuals();
  }
}
```

### 5.3 播放控制

工具栏按钮：
- ▶ Play / ⏸ Pause
- ⏮ Reset
- ⏭ Step（单帧步进）
- 速度：0.25x / 0.5x / 1x / 2x（用于慢动作观察）
- 循环开关

### 5.4 Hitbox 可视化

每个 active hitbox 用 `Handles.DrawWireDisc` 等绘制：
- **半透明颜色**区分类型（蓝色 = 玩家攻击，红色 = 敌方攻击）
- **实线**：active 状态
- **虚线**：lifetime 即将结束
- **闪烁**：刚命中目标

跟随附着骨骼移动（如挥剑）。

### 5.5 Action 视觉提示

- VFX：实际播放（用 Particle System）
- Sound：实际播放（音箱图标提示）
- Camera Shake：相机实际抖动
- Hit Pause：动画播放速度减慢
- Damage Number：弹出数字（基于公式预览）

完整预览所有反馈，所见即所得。

### 5.6 受击假人

- 模拟"被打中" 反应
- 假人配置可调（HP、defense 等）
- 看伤害结算结果
- 可切到"假人会反击"模式（连招测试）

### 5.7 训练对比模式

支持加载两个技能对比：
- 上方播 v1 数据
- 下方播 v2 数据
- 用于平衡前后版本对比

---

## 6. 数据双向同步

### 6.1 加载流程

```
打开技能 → 读 data/generated/skills.bytes 中对应 SkillDef
            ↓
        反序列化为编辑器内存模型 (EditableSkill)
            ↓
        填充 Timeline / Inspector UI
```

### 6.2 保存流程

```
点击 Save → 编辑器内存模型 → 转换为 Excel 行格式
            ↓
        EPPlus 打开对应 .xlsx
            ↓
        修改 Skills / SkillTimelines / 各 Action 表的相关行
            ↓
        保存 .xlsx
            ↓
        触发 DataBuild 增量构建
            ↓
        新 .bytes 生成
            ↓
        预览自动重载
```

**编辑器不直接写 .bytes**——必须经过 Excel + DataBuild 流程，保证一致性。

### 6.3 冲突处理

策划 A 在 Excel 改了 skill 1001，策划 B 同时在编辑器改：
- 编辑器加载时记录"原始版本"hash
- 保存时检查当前 Excel 是否已被改动
- 不一致 → 提示冲突，让用户选择"覆盖 / 合并 / 取消"

### 6.4 Live Reload

DataBuild 完成后，编辑器自动检测 .bytes 变化，重新加载预览。

也支持**Excel 文件 watcher**：策划在 Excel 改 → 编辑器自动重载（如果是当前打开的技能）。

---

## 7. 动画师工作流

### 7.1 Anim Notify 转 Frame Data

动画师在 Unity Animator 配 Animation Event：
- 标注"挥剑接触帧" `OnSwordContact`
- 标注"收招开始帧" `OnRecoveryStart`

工具读取这些 event：
- 自动建议 `SpawnHitbox` 在 `OnSwordContact` 帧
- 自动建议 `cancel_window_open` 在 `OnRecoveryStart` 帧

策划不必手动算帧数。

### 7.2 Root Motion 提取

参见 `MOVEMENT_SYNC.md §8.2`：
- 工具按钮"Extract Root Motion"
- 选择 AnimationClip
- 输出 MovementCurve
- 自动配置 Dash action 引用此曲线

一键完成。

### 7.3 反向：从 Excel 标记到 Animator

如果 Excel 已配置 timeline，工具可反向**在 Animator Window 显示标记**：
- Animator 时间轴上叠加 Atlas timeline 事件
- 动画师可看到"我这个动画在第 X 帧应该出 hitbox"
- 调动画时同步看效果

### 7.4 多动画组合（混合连招）

连招由多段动画组成（attack_1 → attack_2 → attack_3）：
- 工具支持加载多 skill state（参见 `SKILL_SYSTEM.md §4 状态机`）
- 时间轴显示状态切换边界
- 可单独编辑某 state，也可看整体连招感觉

---

## 8. 批量编辑与对比

### 8.1 多技能同时打开

Tab 式打开多个技能：
- 不同技能各自时间轴
- 切换 Tab 快速比对
- 跨技能复制事件（"把这个 hit pause 配置复制给 attack_2"）

### 8.2 模板技能（Skill Template）

策划常发现"这 5 个技能基本一样，只是数值不同"。功能：
- 选定一个技能 → 标记为 Template
- 派生出技能 = template + override 字段
- 改 template 影响所有派生（除非派生覆盖了字段）

类似继承，但仅数据层。

### 8.3 数值搜索 / 替换

跨技能搜索：
- "找所有 hitpause duration > 100 的技能"
- "把所有 atk_power 公式系数 1.5 改为 1.6"

批量操作配合 git diff 可控制风险。

### 8.4 平衡 dashboard

侧栏显示当前职业的：
- 各技能 DPS 估算（基于公式 + 默认目标）
- 总 CD 占比（多少时间在等 cd）
- Hit pause 总时长

让策划在编辑时实时看平衡指标，不用等上线后跑数据。

---

## 9. 性能与可用性

### 9.1 加载性能

| 操作 | 预算 |
|---|---|
| 打开编辑器窗口 | < 1 秒 |
| 加载技能列表 | < 500 ms |
| 打开单个技能 | < 200 ms |
| 切 Tab | < 100 ms |

### 9.2 编辑性能

- 拖拽事件块：60 FPS 流畅
- 实时预览：60 FPS（Unity 编辑器）
- 保存 Excel：< 2 秒（单文件）

### 9.3 错误恢复

编辑器异常情况：
- Excel 被外部锁住 → 提示用户，缓存编辑到本地 .pending 文件
- DataBuild 失败 → 显示具体错误（行号 + 原因），不影响编辑器
- Unity Editor crash → 重启时检测 .pending，恢复未保存改动

### 9.4 Undo / Redo

- 支持 Ctrl+Z / Ctrl+Y
- 撤销栈每技能独立
- 上限 50 步
- 不跨保存边界（保存后清空 undo 栈，避免回滚到磁盘已不存在的状态）

---

## 10. 进阶功能

### 10.1 录制预览视频

按钮"Record"录制当前预览：
- Unity Recorder API 录 60fps 视频
- 包含视觉反馈、相机抖动、VFX
- 用于：盲测对照、bug 报告、平衡 review

### 10.2 自动化测试

工具内执行单元测试：
- 选定一组技能
- 对每个跑预定义场景（打假人 / 暴击 / 反伤）
- 输出报告（是否符合预期）

CI 也跑相同测试。

### 10.3 命中点采样

技能命中假人时记录：
- 命中位置
- 伤害量
- 击退轨迹

多次执行采样，得到"这个技能的命中区域分布"——评估手感。

### 10.4 性能 profile

预览模式选"Performance"：
- 单 tick 各 phase 耗时
- 单事件触发耗时
- 内存分配统计

策划可发现"这个技能 spawn 了 100 个 sub-skill 性能炸"等问题。

### 10.5 一键打包测试

按钮"Send to Test Server"：
- 保存当前 Excel
- 触发 DataBuild
- 上传 .bytes 到测试服
- 自动登入测试账号

策划改完直接验证，免切环境。

---

## 11. v1 / v2 / v3 演进

### 11.1 v1（P2 中）

**最小可用**：
- IMGUI 实现，简朴 UI
- 时间轴 + Inspector + 预览三件
- 基础 action 编辑（Hitbox / Anim / Dash）
- 单技能打开 + 保存

**目标**：策划能用，比 Excel 直接编辑快 5 倍。

### 11.2 v2（P3 早）

**功能完善**：
- UI Toolkit 重构（更专业 UI）
- 全 action 类型支持
- 多 Tab 多技能
- DSL 内联编辑
- 动画师协作流（Anim Notify 集成）
- 录制预览视频

**目标**：单技能调整 30 秒以内。

### 11.3 v3（P4+）

**高级功能**：
- 模板技能 + 派生
- 平衡 dashboard
- 跨技能搜索/替换
- 自动化测试集成
- Performance profile

**目标**：单职业（60 技能）打磨 1 周完成。

### 11.4 不在范围

- 节点编辑器（Visual Scripting）—— 等 P5 视情况上
- 在线编辑（多人协作）—— 复用 git 即可
- 玩家版本编辑器 —— 永不

---

## 12. FAQ 与反模式

### Q1: 为什么不用 Unity Timeline 资源？

Unity Timeline 优势：现成 UI、剪辑动画、cinematic 用得多。劣势：
- 数据格式 Unity 专有，服务端不能用
- 不支持自定义 action 类型扩展
- 性能在 ARPG 战斗实时场景不够

自研 Frame Data Editor 数据走 Atlas 通用管线，保证端同。

### Q2: 用 IMGUI 还是 UI Toolkit？

v1 用 **IMGUI**：
- 学习曲线低，工程师快速实现
- 编辑器原生工作流（Custom Inspector 标准）
- 性能好

v2 切 **UI Toolkit**：
- 更现代，支持 USS/UXML
- UI 复杂度上来后维护性更好

不要在 v1 强求 UI Toolkit——延误工具产能。

### Q3: 编辑器修改的同步会不会影响策划在 Excel 的编辑？

会。所以约定：**同一时刻同一技能只能一处编辑**：
- 策划 A 用编辑器开了 skill 1001
- 策划 B 想在 Excel 改 1001 → 编辑器主动 lock，提示 B 等待

工具内 lock 机制 + git 提交记录辅助。

### Q4: 工具开发成本多大？

估算（参考类似项目）：
- v1 MVP：1 个工程师 4–6 周
- v2 完善：2–3 个月
- v3 高级：持续投入

总计 1 工程师全职 6 个月可达 v3 水准。**这是工程 35–40% 投入工具的应用之一**。

### Q5: 策划用得起来吗，需要培训吗？

需要 1–2 周的"工具培训":
- 视频教程（5 集，每集 10 分钟）
- 文档手册
- 实战练习（让策划改 5 个技能）
- 答疑会议

预期：策划上手后效率 5–10 倍提升。

### Q6: 美术能用这个工具吗？

VFX 美术、动画师可用关键功能：
- 动画师：标注 Anim Notify、提取 Root Motion
- VFX 美术：调整 VFX 触发时机、参数
- 战斗策划：完整 timeline 编辑

权限按角色配置（避免动画师误删 hitbox）。

### Q7: 工具卡 Unity 编辑器吗？

不会—— Editor Window 是隔离的进程内组件：
- IMGUI 渲染轻量
- 数据加载缓存
- 大数据量分页加载

实测目标：开 50 技能 Tab，Unity Editor 帧率 ≥ 30 FPS。

### Q8: 怎么处理工具里的 bug？

工具自身的 bug：
- 内部测试工具，影响范围限内部
- 问题报告 → 工程师修复 → 下版工具
- 严重 bug 阻塞策划工作 → 当天修复

不要让工具 bug 阻塞策划——这违背工具的存在意义。

### Q9: 工具是否进游戏发布包？

**不进**。工具仅在 Unity Editor 中可用，不打入 player build：
- `[InitializeOnLoad]` 编辑器宏隔离
- 文件夹 `Assets/Editor/AtlasFrameDataEditor/` 自动排除
- 玩家版本无访问途径

### Q10: 工具失败时策划怎么继续工作？

退化路径：直接编辑 Excel + DataBuild + 启动游戏 → 这是工具加持前的工作流，仍然可用。**工具是加速器不是必需品**——必需品是 Excel + DataBuild。

---

### 反模式清单

- ❌ 编辑器直接写 .bytes 跳过 Excel（破坏数据真源）
- ❌ 工具私有数据格式不进 git（与团队协作冲突）
- ❌ 让工具变成"半个游戏"（功能膨胀偏离编辑核心）
- ❌ 在工具里加业务逻辑（工具改 → 业务改 = 双轨）
- ❌ 工具产生的代码进游戏发布包
- ❌ 用工具绕过 DataBuild 校验（错数据上线 bug）
- ❌ 工具版本与 DataBuild 版本不同步（产物不兼容）
- ❌ 工程师觉得"工具够用了不再迭代"（策划永远有新需求）

---

## 13. 里程碑

| 阶段 | 交付 |
|---|---|
| P0 末 | 启动工具开发；选定技术栈（IMGUI v1） |
| P1 中 | v1 MVP：单技能加载 / 时间轴显示 / 基础编辑 |
| P1 末 | v1 完整：所有 action 类型支持；Excel 双向同步 |
| **P2 早** | **v1 上线：策划开始用其产出 5 个测试技能** |
| P2 中 | v2 启动：UI Toolkit 重构 |
| P2 末 | v2 完善：DSL 内联编辑 / 动画师协作流 |
| P3 | 录制预览视频功能 / 平衡 dashboard 雏形 |
| P4+ | v3 高级功能持续迭代 |

---

## 14. 文档维护

- **Owner**：Tools Engineer + Combat Designer Lead 共担
- **关联文档**：
  - `OVERVIEW.md`（§7 难点五 引用本文）
  - `MILESTONE_COMBAT_FEEL.md`（手感测试录制依赖工具）
  - `SKILL_SYSTEM.md`（编辑对象的数据 schema）
  - `COMBAT_FEEL.md`（编辑器预览反馈）
  - `COMBAT_ACTIONS.md`（每种 action 的字段）
  - `DATA_PIPELINE.md`（编辑器与构建的关系）
  - `MOVEMENT_SYNC.md §8`（Root Motion 提取）

---

**文档结束。**

**核心纪律重申**：
1. **Excel 是真源**：编辑器只是 GUI 包装
2. **所见即所得**：预览必须真实反映运行时
3. **改 → 看 ≤ 3 秒**：手感迭代的关键
4. **工具不停迭代**：策划反馈每周收集，每月小版本
5. **不替代 Excel**：策划仍然可以直接编辑 Excel（兜底）
