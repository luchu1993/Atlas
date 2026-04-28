# 战斗手感设计（Combat Feel）

> **用途**：定义 Atlas 实现"超越 BDO 手感"目标所需的视听反馈系统——Hit Pause、相机效果、动画驱动位移、受击反馈分层。这是 P3 里程碑成功与否的核心。
>
> **读者**：工程（必读）、战斗策划（必读）、动画师（§5、§7 必读）、特效美术（§6 必读）、音效设计（§9 必读）。
>
> **状态**：草案 v0.1 — 待团队评审。
>
> **前置文档**：`OVERVIEW.md`、`MILESTONE_COMBAT_FEEL.md`、`SKILL_SYSTEM.md`、`COMBAT_ACTIONS.md`
>
> **关联文档**：`MOVEMENT_SYNC.md`（动画驱动位移）、`HIT_VALIDATION.md`（命中事件源头）、`BUFF_SYSTEM.md`（CC 反馈）

---

## 1. 为什么手感是核心

### 1.1 BDO 凭什么甩开同类 MMO

很多 MMOARPG 在网络/玩法/数值/美术单项都不输 BDO，但**手感**让 BDO 一骑绝尘。拆开看：

- **打击瞬间的物理感**：刀刃碰到肉的"挫"感，不是穿透
- **相机参与战斗**：每次重击观众也"被打了一下"
- **动画与位移融合**：挥剑前冲的弧线感
- **反馈层次清晰**：普攻轻、技能中、大招重，差异肉眼可辨

这些**不是单点技术问题，是系统化设计**——Atlas 要超越，必须从架构层把这些原语作为一等公民。

### 1.2 P3 的 4.0 评分从哪来

参见 `MILESTONE_COMBAT_FEEL.md §3.2`，**打击感（Impact Feel）权重 30%**——五维度中最大。本文是该维度的具体技术实现。

### 1.3 设计原则

1. **手感原语化**：把 hit pause / camera / animation-driven 抽象成 Action，可被任何技能复用
2. **数据驱动分层**：不同等级技能用不同强度的反馈组合，由 Excel 配置
3. **客户端独立**：手感反馈是纯视觉/听觉，不影响服务端权威
4. **不抢玩家控制权**：相机效果可关闭/可调节强度（无障碍设置）
5. **量化可测**：每项反馈有可测量参数，便于 A/B 调优

---

## 2. 总体架构

### 2.1 反馈分层

每次"成功命中"事件触发**多层反馈**，层间独立又协同：

```
Damage Event 发生（服务端权威）
     ↓ 广播
客户端收到事件，按事件 metadata 触发反馈层：
  ├─ 层 1: Hit Pause（攻守双方动画暂时减速）
  ├─ 层 2: Camera Shake（相机震动）
  ├─ 层 3: Camera Slomo（短暂慢镜头，仅大招）
  ├─ 层 4: VFX（粒子、屏幕特效）
  ├─ 层 5: Audio（多层音效：挥风/撞击/低频共鸣）
  ├─ 层 6: Hit Reaction Animation（受击者播放反应动画）
  ├─ 层 7: Damage Number（伤害数字弹出）
  └─ 层 8: Controller Vibration（手柄震动，如适用）
```

每层独立可调，组合形成"打击感"。

### 2.2 反馈强度档位

定义 4 档（数据驱动），不同技能匹配不同档：

| 档 | 用途 | Hit Pause | Camera Shake | Slomo | VFX | Audio |
|---|---|---|---|---|---|---|
| **Light** | 普攻第 1 段 | 30 ms | 微弱 | 无 | 小粒子 | 单层 |
| **Medium** | 普攻末段 / 技能 | 50 ms | 中等 | 无 | 中粒子 | 双层 |
| **Heavy** | 大技能 / 重击 | 80 ms | 强 | 无 | 大粒子 | 三层 |
| **Ultimate** | 终结/必杀 | 120 ms | 强烈+方向 | 200 ms × 0.5 | 全屏特效 | 全混音 |

**纪律**：策划在配置技能时**只选档**（`feedback_tier: Light/Medium/Heavy/Ultimate`），具体参数从档表 lookup。这避免每个技能调一堆参数。

### 2.3 反馈触发时机

反馈不是在"伤害结算瞬间"触发，而是按命中事件流：

```
t=0       SpawnHitbox（在 timeline 配置时间）
t=2-5ms   服务端检测命中（hit detect）
t=5-50ms  事件传到客户端（网络延迟）
t+       客户端反馈层并行启动:
          ├ Hit Pause: 立即
          ├ VFX: 立即  
          ├ Audio: 立即（最关键，0 延迟感）
          ├ Camera Shake: 立即
          ├ Hit Reaction: ~30ms 后（先放预备帧）
          ├ Damage Number: 100-200ms 后（让玩家先感受打击）
          └ Slomo: 仅 Ultimate，立即
```

---

## 3. Hit Pause（顿帧）★

### 3.1 概念

命中瞬间，**攻击者和受击者的动画时间短暂减速或冻结**，同时世界其他物体正常运行。

视觉效果：玩家感觉"碰到东西了"——不是穿透，是物理碰撞。

### 3.2 实现策略

**服务端**：完全不变。仿真按原速继续。

**客户端**：调整 Animator 的播放速率。

```csharp
public sealed class HitPauseSystem : MonoBehaviour {
  public void TriggerPause(Animator animator, float timeScale, int durationMs) {
    StartCoroutine(PauseAnimator(animator, timeScale, durationMs));
  }
  
  IEnumerator PauseAnimator(Animator anim, float scale, int durationMs) {
    float originalSpeed = anim.speed;
    anim.speed = originalSpeed * scale;
    yield return new WaitForSeconds(durationMs / 1000f);
    anim.speed = originalSpeed;
  }
}
```

**作用范围**（`HitPauseAction.target_scope`）：
- **AttackerOnly**：仅攻击者动画暂停（"打到墙上"的反震感）
- **VictimOnly**：仅受击者动画暂停（"被打懵"）
- **Both**：双方都暂停（**最常用**，最强打击感）

### 3.3 不冻结的东西

Hit pause 期间**不冻结**：
- 玩家输入（仍接受按键，但应用延后到 pause 结束）
- 相机控制
- UI 更新
- Particle System（VFX 继续播放，否则反馈中断）
- 其他角色的动画

仅冻结**直接参与本次命中的双方动画**。

### 3.4 时间值规范

| 档 | duration_ms | time_scale |
|---|---|---|
| Light | 30 | 0.3 |
| Medium | 50 | 0.1 |
| Heavy | 80 | 0.05 |
| Ultimate | 120 | 0 (完全冻结) |

**为什么不全用 0**：
- 完全冻结显得"死"
- 微速运动保留生命感
- 仅 Ultimate 用 0 制造极致冲击

### 3.5 多重命中处理

一个 AoE 一次命中 5 个目标：
- 触发 5 个 Hit Pause 事件
- 但 attacker 只一份，**取最强档**（Ultimate > Heavy > Medium > Light）
- Victim 各自独立

### 3.6 与连招的关系

连招中段命中触发 Hit Pause **不能阻塞连招输入**：
- 玩家在 Hit Pause 期间按下"下一招"键
- 输入进入缓冲队列
- Pause 结束立即读取缓冲，开始下一招
- 玩家感知：节奏丝滑

这要求 SkillRunner / Animator 的"输入采样"和"动画播放"解耦——Hit Pause 只影响后者。

### 3.7 实现位置

通过 `HitPause` action（见 `COMBAT_ACTIONS.md §3.10`）：

```
SkillTimeline (timeline 中的 hitbox 命中后回调):
  Hitbox.on_hit_dsl_ref:
    if target.is_alive {
      emit HitPause(target_scope=Both, duration_ms=tier_lookup, time_scale=tier_lookup);
      emit CameraShake(...);
      emit PlayVFX(...);
      emit DamageTarget(target, dmg);
    }
```

**纪律**：HitPause 必须在 DamageTarget **之前** emit，这样玩家先感觉"打到了"，再看伤害数字（顺序很关键）。

---

## 4. 相机效果

### 4.1 Camera Shake

**参数**：
| 字段 | 含义 |
|---|---|
| `intensity` | 振幅（米） |
| `duration_ms` | 持续 |
| `frequency_hz` | 振动频率 |
| `falloff_curve` | 衰减曲线（Linear / Exp / EaseOut） |
| `axis_weight` | 各轴权重（vec3） |

**实现思路**（伪代码）：

```csharp
public sealed class CameraShakeSystem : MonoBehaviour {
  Vector3 _accumOffset;
  
  public void Trigger(float intensity, int durationMs, float freq, 
                      AnimationCurve falloff, Vector3 axisWeight) {
    StartCoroutine(ShakeRoutine(intensity, durationMs, freq, falloff, axisWeight));
  }
  
  IEnumerator ShakeRoutine(...) {
    float elapsed = 0;
    float total = durationMs / 1000f;
    while (elapsed < total) {
      float t = elapsed / total;
      float current = intensity * falloff.Evaluate(1 - t);
      
      // Perlin noise 平滑振动
      Vector3 offset = new Vector3(
        Mathf.PerlinNoise(elapsed * freq, 0) - 0.5f,
        Mathf.PerlinNoise(elapsed * freq, 100) - 0.5f,
        Mathf.PerlinNoise(elapsed * freq, 200) - 0.5f) * current * 2;
      
      _accumOffset += Vector3.Scale(offset, axisWeight);
      elapsed += Time.unscaledDeltaTime;
      yield return null;
    }
  }
  
  void LateUpdate() {
    Camera.main.transform.position += _accumOffset;
    _accumOffset = Vector3.Lerp(_accumOffset, Vector3.zero, 0.5f);  // 衰减
  }
}
```

**关键**：用 `unscaledDeltaTime`，**不受 Hit Pause 的 timeScale 影响**——shake 永远按真实时间走。

### 4.2 强度档（与 Tier 联动）

| 档 | intensity | duration | freq | axis |
|---|---|---|---|---|
| Light | 0.05 m | 80 ms | 30 Hz | 主导垂直 |
| Medium | 0.12 m | 150 ms | 25 Hz | 平衡 |
| Heavy | 0.25 m | 250 ms | 20 Hz | 主导冲击方向 |
| Ultimate | 0.4 m | 400 ms | 15 Hz | 多方向组合 |

**axis_weight** 让 shake 有"方向"——比如水平劈斩主要左右晃，纵向重击主要上下晃。

### 4.3 多 Shake 叠加

同时触发多个 shake：
- 偏移**线性叠加**
- 但有**总幅度上限**（防溢出 0.6m，超出 clamp）
- 每个 shake 独立衰减

### 4.4 Camera Slomo

仅 Ultimate 使用。

**参数**：
| 字段 | 含义 |
|---|---|
| `time_scale` | 慢镜头倍率（0.3-0.7） |
| `duration_ms` | 持续 |
| `ease_in_ms` | 进入过渡 |
| `ease_out_ms` | 退出过渡 |

**实现**：调整 Unity `Time.timeScale`：

```csharp
public IEnumerator SlomoRoutine(float scale, int duration, int easeIn, int easeOut) {
  float originalScale = Time.timeScale;
  
  // ease in
  for (float t = 0; t < easeIn / 1000f; t += Time.unscaledDeltaTime) {
    Time.timeScale = Mathf.Lerp(originalScale, scale, t / (easeIn / 1000f));
    yield return null;
  }
  
  // hold
  Time.timeScale = scale;
  yield return new WaitForSecondsRealtime(duration / 1000f);
  
  // ease out
  for (float t = 0; t < easeOut / 1000f; t += Time.unscaledDeltaTime) {
    Time.timeScale = Mathf.Lerp(scale, originalScale, t / (easeOut / 1000f));
    yield return null;
  }
  
  Time.timeScale = originalScale;
}
```

**重要**：Time.timeScale 影响整个客户端时间（动画/物理/特效），但**不影响服务端**——服务端永远全速。

**端同问题**：玩家慢镜头时，服务端事件继续到达，客户端**用本地缓冲队列**滞留事件，等 slomo 结束再统一播放（保持视觉一致）。

### 4.5 Camera Focus（构图）

Ultimate 可触发**镜头切换**：
- 拉近主角
- 旋转到合适角度
- 持续到 Ultimate 动画结束
- 退出时平滑过渡回原位

**参数**：
| 字段 | 含义 |
|---|---|
| `target_dsl_ref` | 聚焦目标 |
| `framing` | TightOnTarget / WideOnGroup / OverShoulder |
| `duration_ms` | 持续 |
| `transition_ms` | 进入/退出过渡 |

实现走 Unity Cinemachine（`CinemachineBlendingCamera` 切换 vcam）。

### 4.6 多人 PvP 中的相机

PvP 1v1 / 2v2 / 4v4 中，**每个玩家相机独立**：
- 玩家 A 释放 Ultimate 触发 slomo → **只对 A 生效**
- 其他玩家相机正常
- 服务端事件在所有客户端到达，但 slomo 是客户端本地"享受"

**例外**：观战模式可让观战者的相机也跟随 slomo（增强观赏性）。

### 4.7 无障碍 / 设置

玩家可在设置中：
- `Camera Shake Intensity`：0% – 100% 滑条（无障碍考虑）
- `Slomo Enabled`：开关（晕动症患者关闭）
- `Hit Pause Strength`：0% – 100%

设置实时生效。**所有反馈系统都必须接入此设置**（不要硬编码强度）。

---

## 5. 动画驱动位移（Animation-Driven Movement）

详见 `MOVEMENT_SYNC.md §8`，本节仅讨论**动画师工作流**。

### 5.1 概念

某些技能动画包含位移（如挥剑前冲）。如果用 Unity 运行时 Root Motion：
- 端同破坏（Animator 不一致）
- 网络同步困难
- 调试不可控

**Atlas 方案**：构建期从 AnimationClip 提取位移曲线为数据，运行时端同使用曲线。

### 5.2 动画师工作流

1. 动画师在 Maya / Blender 制作动画，包含 root bone 位移
2. 导入 Unity，配置 AnimationClip 的 root motion
3. 运行 `tools/extract_root_motion.py`：
   - 输入：AnimationClip + 关键帧采样率
   - 输出：`MovementCurve` 数据（每帧位移采样）
4. 数据写入 `MovementCurves.xlsx`，分配 `curve_id`
5. 技能配置 `Dash` action 时引用该 `curve_id`
6. **Unity 中关闭 Animator 的 root motion 标志**（动画只播放，位移走 Atlas 系统）

### 5.3 同步保证

服务端 + 客户端用同曲线 → 同 elapsed_ms → 同位置。无需位置流同步。

详见 `MOVEMENT_SYNC.md §8.5–§8.6`。

### 5.4 Hit Window 与位移结合

挥剑前冲技能：
- 位移期间 hitbox 也存在（剑碰到的人都被击中）
- 需要 timeline 同时配 `Dash` 和 `SpawnHitbox`：
  ```
  t=0:   PlayAnim("forward_slash"); Dash(curve_forward, distance=3, duration=400)
  t=80:  SpawnHitbox(capsule_along_forward_path, lifetime=320)
  ```
- 服务端按权威曲线移动 caster，hitbox 随 caster 移动，扫到的目标即被击中

---

## 6. 受击反馈系统（Hit Reaction）

### 6.1 问题

简单做法：所有受击播一个"硬直" 动画。问题：
- 不区分轻重
- 不区分方向
- 不区分武器类型
- 看起来"假"

BDO 之所以"打肉",在于**反应动画极度细分**。

### 6.2 数据驱动的 Reaction Matrix

```
HitReactions.xlsx
| reaction_id | damage_type | direction | victim_state | anim_state | duration_ms |
| 1001        | Slash       | Front     | Standing     | hit_slash_f| 350         |
| 1002        | Slash       | Back      | Standing     | hit_slash_b| 350         |
| 1003        | Slash       | Front     | Jumping      | hit_air_b  | 600         |
| 1004        | Slash       | Front     | SuperArmor   | hit_flinch | 100         |
| 1005        | Blunt       | Front     | Standing     | hit_blunt  | 500         |
| ...         |             |           |              |            |             |
```

**servers selects**：服务端在伤害结算时按 victim 的当前状态 + damage type + direction 查表，得到 `reaction_id`，附加到 `Damage` 事件中。

### 6.3 客户端播放

收到 Damage 事件：
- 读 `reaction_id`
- 切到对应 Animator state
- 持续 `duration_ms` 后回 Idle

**特殊**：
- 受击期间玩家输入被吞（除非有取消窗口）
- Iframe / SuperArmor 时 reaction_id = `1004`（轻闪不打断动作）

### 6.4 倒地与起身

重击 / 击飞导致倒地：
- 服务端施加 Buff `KnockedDown`（持续到玩家按键起身或自动）
- 客户端播放倒地动画
- 起身按键由玩家输入触发，发送 `GetUpInput` 事件
- 服务端确认，移除 KnockedDown buff
- 客户端播放起身动画

倒地期间不可被普通技能命中（部分技能可，由命中判定层处理）。

### 6.5 浮空连击（Juggle）

参见 `BUFF_SYSTEM.md` 浮空机制 + `MOVEMENT_SYNC.md` Launch action：
- 命中导致浮空 → `Launch` action 施加抛物线
- 浮空状态 = `Buff.Airborne`
- 浮空中可被连击（reactions 选 `victim_state=Airborne` 行）
- 落地 → `KnockedDown` 或 `Recover`（看 buff 配置）

---

## 7. 动画规范（Frame Data Standards）

### 7.1 Anticipation / Action / Recovery 三段

每个攻击动画必须包含：

```
┌──────────────────────────────────────────────────┐
│  Anticipation (起手)  │ Action (出招) │ Recovery (收招) │
│  眉头皱、往后蓄力      │  挥砍/释放    │  收回起势 / 喘息  │
│  Frame 0–20%          │  20%–50%      │  50%–100%       │
└──────────────────────────────────────────────────┘
```

策划/动画师配合：
- **Anticipation 帧 = 起手 telegraph**：远端观察者反应窗口
- **Action 帧 = hitbox 生效窗口**：技能伤害判定时机
- **Recovery 帧 = 取消窗口**：连招接续点

每个技能在 `SkillTimelines.xlsx` 配置：
- `anticipation_end_ms`
- `active_start_ms` / `active_end_ms`
- `cancel_window_start_ms` / `cancel_window_end_ms`

### 7.2 帧数对照（30 FPS 思维）

| 阶段 | 推荐帧数 | 毫秒 |
|---|---|---|
| 普攻 anticipation | 3-5 | 100-170 |
| 普攻 active | 2-4 | 70-130 |
| 普攻 recovery | 5-8 | 170-270 |
| 重击 anticipation | 8-15 | 270-500 |
| 重击 active | 4-8 | 130-270 |
| 重击 recovery | 10-20 | 330-670 |
| Ultimate anticipation | 15-30 | 500-1000 |
| Ultimate active | 6-12 | 200-400 |
| Ultimate recovery | 20-40 | 670-1300 |

**纪律**：所有数据存 `SkillFrameData.xlsx`，**Frame Data Editor 工具**可视化展示（见 `09_tools/FRAME_DATA_EDITOR.md`）。

### 7.3 动画师与策划协作流程

```
策划提需求 ──► 动画师做毛坯 (~150 帧动画)
              ↓
          动画师导入 Unity, 在 Frame Data Editor 标注三段
              ↓
          数据存 Excel (active_start_ms 等)
              ↓
          策划在 Editor 里 "试打"
              ↓
       不满意 → 调整毫秒值（不需要重做动画）
       满意 → 锁定，进入数值平衡
```

**关键**：帧数据**用毫秒不用帧号**——动画速度可调，但毫秒值是端同契约。

---

## 8. VFX 设计

### 8.1 分层

每次命中 VFX 由 4 层组成：

```
1. Hit Spark      (打击瞬间的火花/光斑)         ~100ms 短促
2. Impact Burst   (冲击波形粒子)                ~200ms 中等
3. Damage Trail   (向四周扩散的能量痕迹)        ~400ms 渐隐
4. Ambient Glow   (持续光晕，强调位置)          ~600ms 缓慢消失
```

普攻可能只有层 1+2，Ultimate 全部 4 层 + 全屏震动。

### 8.2 与 Hit Pause 同步

VFX 不受 Hit Pause 的 timeScale 影响（VFX 永远全速，否则反馈断裂）。

实现：Particle System 的 `Custom Time Scale` 配置为独立时间源。

### 8.3 性能预算

- 单次命中 VFX 总粒子数 ≤ 200（普攻）/ 500（Heavy）/ 2000（Ultimate）
- 同屏 VFX 总粒子数 ≤ 5000（密集战场上限）
- 超出时按距离剔除

### 8.4 颜色与元素一致性

每种武器/元素一套色调：
- 物理 / 钢铁 → 白银火花
- 火 → 橙红
- 冰 → 浅蓝白
- 雷 → 紫蓝
- 暗 → 紫黑
- 圣 → 金白

策划配置技能时选 `vfx_palette: enum`，VFX 系统按 palette 替换粒子贴图色调。**避免每个技能各做一套 VFX**。

---

## 9. 音效设计

### 9.1 多层音效

每次命中音效由多层叠加：

| 层 | 内容 | 时长 | 时机 |
|---|---|---|---|
| 1 | 武器挥风 (whoosh) | ~100ms | active_start 之前 |
| 2 | 命中接触 (impact) | ~50ms | 命中瞬间 |
| 3 | 受击者反应 (grunt/scream) | ~300ms | 命中后 30ms |
| 4 | 低频共鸣 (deep boom) | ~500ms | 命中瞬间（仅 Heavy/Ultimate） |
| 5 | 拖尾余韵 (resonance) | ~800ms | 命中后渐淡 |

每层独立 audio clip，独立音量曲线。

### 9.2 不同武器分类

每种武器分 5 层全部独立采样：
- 单手剑 / 双手剑 / 长矛 / 锤 / 弓 / 法杖 / 拳套...
- 每种重击/普攻/暴击各一套
- ~200 个 audio clip 起步

工作量大，但**音效是手感的隐形主角**——不投入这部分手感感觉永远不对。

### 9.3 PvP 中的"听音辨位"

PvP 玩家听技能音效判断敌人位置/动作：
- 3D 空间音效（必须）
- 音量按距离衰减
- 重要技能（必杀技起手）即使被障碍物挡住仍可听到（衰减弱）——平衡公平

### 9.4 实现

通过 `PlaySound` action（见 `COMBAT_ACTIONS.md §3.10`）：
- 服务端 emit 事件
- 客户端按 `sound_id` + `attach_to_caster` 播放
- 多层音效 = timeline 多个 `PlaySound` event

---

## 10. UI 反馈

### 10.1 伤害数字

**风格分层**：

| 档 | 字号 | 颜色 | 动画 | 时机 |
|---|---|---|---|---|
| Light | 18pt | 白 | 上飘 | 100ms 后 |
| Medium | 24pt | 黄 | 上飘 + 抖 | 100ms 后 |
| Heavy / Crit | 32pt | 橙 | 弹出 + 抖 + 拖尾 | 150ms 后 |
| Ultimate | 48pt | 红金渐变 | 大幅弹出 + 全屏闪 | 200ms 后 |

**关键时机**：伤害数字**晚 100-200ms** 才显示——让玩家先感受打击（hit pause + VFX + audio），再看数字。这是 BDO 的细节，立即显示反而打散反馈。

### 10.2 屏幕周边反馈

- **被击红边**：受到伤害时屏幕边缘红色脉冲（强度按伤害比例）
- **低血警告**：HP < 20% 时屏幕变暗 + 心跳音效
- **暴击全屏**：暴击瞬间全屏淡黄闪一帧
- **击杀确认**：杀掉敌人时屏幕中心小图标 + 音效

### 10.3 命中确认（Hit Confirmation）

PvP 中"我打到了吗"的关键反馈：
- 命中：准星/瞄准点变红色，伴随短音效
- 暴击：准星脉冲 + 不同音效
- 击杀：准星变金色 + 击杀音效

这给玩家**确定性反馈**——不再依靠观察伤害数字。

---

## 11. CC（控制）反馈

### 11.1 进入 CC

被 stun / freeze / knockdown 时：
- **屏幕中心提示**：图标 + 文字（"Stunned 1.5s"）
- **角色身体 VFX**：电流（stun）、冰晶（freeze）、星星（confuse）
- **Audio 提示**：CC 类音效（钝击声、冰冻声）
- **Controller 反馈**：手柄强振动

### 11.2 CC 解除

- 屏幕提示消失
- VFX 渐隐
- 玩家立即可输入

### 11.3 PvP 中的 CC 重要性

竞技场中误判 CC 状态 = 输——必须强反馈。Atlas 设计：
- CC 进入瞬间用最强音效之一（不能错过）
- 屏幕中心 CC 计时条
- 倒计时清晰可读

---

## 12. 端同与确定性

### 12.1 视觉反馈不进确定性契约

所有 Combat Feel 反馈都是**纯客户端**：
- 服务端不执行 HitPause / CameraShake / VFX / Sound
- 服务端只广播事件
- 客户端各自渲染（多个客户端可看到略有差异，无所谓）

### 12.2 但触发源必须确定性

哪怕反馈是视觉，**触发反馈的事件**必须按确定性流程：
- Damage 事件按 `COMBAT_EVENT_ORDERING` 排序
- 反馈强度档（`feedback_tier`）从技能数据读，不靠客户端估算

### 12.3 客户端预测的反馈

主控玩家释放技能：
- 客户端立即播放挥剑动画（预测）
- Hit Pause 在收到服务端 hit 事件后触发（确认）
- 若服务端拒绝（防御无效），不补播 Hit Pause（玩家明白"打空"）

---

## 13. 测试与验证

### 13.1 P3 盲测对照（参见 `MILESTONE_COMBAT_FEEL.md`）

测试者打分中**打击感占 30%**——本系统的实现质量直接决定 P3 分数。

### 13.2 量化指标

每个反馈层有可测指标：
- Hit Pause：实测 timeScale 变化曲线，对比设计值
- Camera Shake：振动幅度峰值与时间曲线
- Audio：多层 clip 的触发时间精度（±5ms）
- VFX：粒子数 / 持续时间符合配置

工具：自动化 frame-by-frame 录制 + 对比设计基线。

### 13.3 手感回归测试

每个版本对**TOP 20 标杆技能**录制 60fps 视频：
- 手动对比上版本
- 量化指标自动 diff
- 显著回归（>5% 偏差）必须解释

### 13.4 参考库（Reference Library）

收集**业界标杆**的视频片段：
- BDO Striker 暴击
- Dragon Nest Sword Master 三段
- Bayonetta combo
- DMC5 Nero
- Sekiro 弹反
- Soulsborne 重击

定期对比 Atlas 实现，看差距在哪。

---

## 14. FAQ 与反模式

### Q1: Hit Pause 会让 PvP 不公平吗（一方暂停一方没暂停）？

**不会**——Hit Pause 在两人客户端各自触发（双方都收到 Damage 事件，各自执行 HitPause action）。Time scaling 是客户端独立的，**服务端永远全速**——连招接续判定走服务端 tick，不受 pause 影响。

### Q2: Slomo 期间玩家输入怎么处理？

按 `unscaledDeltaTime` 采集，**不被 timeScale 影响**——玩家在慢镜头里仍能正常操作。这避免"被强制慢动作"的体验。

### Q3: 200 人战场，每个命中都触发 shake 不会爆炸吗？

会。所以**距离过滤**：
- 距离 > 30m 的命中不触发本地相机 shake
- 距离 < 30m 的事件按距离衰减强度
- 超出 5 个并发 shake 时合并叠加（不无限叠）

### Q4: 受击 Animation 太多，要做几百个？

**初期不需要全套**：先做 4-6 种核心 reaction（前/后劈砍 + 钝击 + 浮空），其他用基础 reaction 替代。**P4 之后**根据玩家反馈逐步细分。

### Q5: VFX/audio 资源量大，怎么管理？

- 模板化：每种武器一套基础模板，色调和强度按数据替换
- 复用：不要每个技能都做独占 VFX
- 流式加载：进入战斗前预加载技能用到的资源

### Q6: 伤害数字延迟 100-200ms 显示，玩家不会觉得"延迟"吗？

不会——**延迟的是数字 UI 出现**，不是反馈本身。玩家先看到 hit spark + 听到 impact + 感觉到 hit pause，这些是 0 延迟的。数字只是"伤害量化"的辅助信息。BDO 经验证明此时机最佳。

### Q7: Camera Shake 滑条用户调到 0 怎么办？

完全关闭。某些晕动症玩家必须如此。不要在数据层强制开启——那是无障碍设计禁忌。但**不影响游戏平衡**，shake 是纯视觉。

### Q8: Hit Pause 和动画切换冲突怎么办？

Hit Pause 期间收到"切到下一招" 输入：
- 输入进 buffer
- Pause 结束后立即切动画 + 应用积压输入
- Animator state machine 必须设计成支持瞬间切换（用 trigger 而非状态判断）

### Q9: 客户端预测的 hit spark 和服务端否认怎么办？

**不预测 hit spark**——除非客户端有完全把握命中（如对训练假人）。PvP 中等服务端确认再触发 spark，多 50ms 延迟可接受。

宁可"略晚反馈"，不要"假反馈被收回"。

### Q10: 怎么知道当前手感够不够好？

**只有玩才知道**。每周一次"手感会议"：
- 团队成员盲测对手已上线 MMOARPG
- 然后试自己的版本
- 直接说"哪里不对"
- 不舒服处记录，下周看是否改进

数据 + 主观，缺一不可。

---

### 反模式清单

- ❌ Hit Pause 全部冻结相机（玩家以为游戏卡了）
- ❌ Camera Shake 强度溢出 1m（玩家完全失去视野）
- ❌ Slomo 在 PvP 实时战斗中触发（团战观赛体验破坏）
- ❌ 所有技能用同一套反馈（无层次感）
- ❌ 反馈强度硬编码到代码（应数据驱动）
- ❌ 伤害数字 0ms 立即弹（节奏过早）
- ❌ 服务端执行 HitPause（混淆责任，浪费 CPU）
- ❌ VFX 受 Time.timeScale 影响（反馈断片）
- ❌ Audio 命中音单层（缺乏层次）
- ❌ 受击动画一种通用（"假" 的源头）

---

## 15. 里程碑

| 阶段 | 交付 |
|---|---|
| P2 中 | HitPause / CameraShake / VFX / PlaySound action 实现 |
| P2 末 | 4 档反馈 tier 数据驱动；3 种基础受击动画 |
| **P3 早** | **完整反馈系统：Slomo / CameraFocus / Hit Reaction Matrix** |
| P3 中 | 完整 1 个职业 20 技能反馈打磨 |
| **P3 末** | **盲测达标（PvE 4.0 / PvP 3.5），手感专项 ≥ 4.0** |
| P4+ | 多职业反馈个性化；高级动画细节 |

---

## 16. 文档维护

- **Owner**：Tech Lead + Combat Designer + Animation Lead 三方共担
- **关联文档**：
  - `MILESTONE_COMBAT_FEEL.md`（验证标准）
  - `SKILL_SYSTEM.md`（手感反馈通过 timeline action 触发）
  - `COMBAT_ACTIONS.md`（HitPause/CameraShake/VFX/PlaySound action 定义）
  - `BUFF_SYSTEM.md`（CC 反馈的状态来源）
  - `HIT_VALIDATION.md`（命中事件源头）
  - `MOVEMENT_SYNC.md §8`（动画驱动位移）
  - `09_tools/FRAME_DATA_EDITOR.md`（动画数据编辑工具）

---

**文档结束。**

**核心纪律重申**：
1. **手感是系统性的**，不是单点优化
2. **分层反馈**：每次命中调用多层独立系统
3. **客户端独占**：服务端不参与视觉反馈
4. **数据驱动 4 档**：策划只选档，不调具体参数
5. **量化测量 + 主观盲测**：两手都要有
6. **超越 BDO 不是口号**：每周对比标杆，每个细节较真
