# 服务端物理架构设计

> **用途**：定义 Atlas 作为工业化 MMO 服务器引擎时，服务端物理子系统的长期边界、资源管线、运行时集成、Jolt 后端策略和分布式扩展约束。
>
> **读者**：服务端工程、客户端/Unity 工程、工具链工程、战斗系统工程、技术策划。
>
> **状态**：草案 v0.1 — 待团队评审。
>
> **前置文档**：`docs/gameplay/OVERVIEW.md`、`docs/gameplay/01_world/CELL_ARCHITECTURE.md`、`docs/gameplay/02_sync/MOVEMENT_SYNC.md`、`docs/gameplay/03_combat/HIT_VALIDATION.md`

---

## 1. 定位

Atlas 的物理系统是 **MMO 服务器引擎基础设施**，不是某个项目的玩法脚本，也不是第三方物理库的薄封装。它需要长期服务于：

- 开放世界与副本并存的 Space / Cell 架构
- Unity 客户端内容生产管线
- 服务端权威移动、技能、投射物、触发器、视线和地形查询
- 热点 PvE 与小规模高公平 PvP
- 长期版本升级、多项目复用、可观测运维和工具化生产

核心决策：

```text
Atlas 拥有物理架构；Jolt 是首个生产级后端。
```

这意味着上层系统依赖 Atlas 自己的物理 API、资源格式和运行时服务，而不是直接依赖 Jolt 类型或 Jolt cooked binary。

---

## 2. 设计目标

### 2.1 必须满足

1. **服务端权威**：移动、碰撞阻挡、技能命中、投射物命中、触发器判定以服务端结果为准。
2. **后端隔离**：gameplay、server、script 层不 include Jolt 头，不存储 Jolt 句柄。
3. **资源管线稳定**：Unity 是 authoring 来源，Atlas collision asset 是长期源资产，Jolt cache 是可重建派生物。
4. **固定 tick 集成**：所有动态物理状态同步、查询、触发器评估发生在明确 tick 阶段。
5. **分布式可演进**：第一版可单 Space/Scene，接口不能阻断未来 chunk、Cell 边界和跨 Cell 查询策略。
6. **可观测**：查询次数、耗时、body 数量、内存、cache 命中、慢查询必须进入 Watcher 和 Tracy。
7. **可测试**：保留 Null/Test backend，核心移动和战斗测试不强依赖 Jolt runtime。
8. **可控成本**：脚本和玩法查询必须带 layer、mask、filter、结果上限和预算。

### 2.2 非目标

- 不把 Atlas 变成完整刚体沙盒。
- 不让角色权威移动依赖 Jolt 的现成 CharacterController。
- 不用动态刚体碰撞事件作为技能命中的唯一来源。
- 不追求 Unity PhysX 与 Jolt bit-level 一致。
- 不把 Jolt cooked binary 作为唯一地图碰撞资产。
- 不在脚本层暴露底层物理对象的直接可变访问。

---

## 3. 总体架构

```text
Unity Authoring
  - ServerColliderAuthoring
  - ServerVolumeAuthoring
  - Validate / Preview / Export
        │
        ▼
Atlas Collision Asset
  - stable source asset
  - debug dump
  - versioned metadata
        │
        ▼
atlas_tool validate/cook
  - mesh cleanup
  - layer/material mapping
  - chunking
  - backend cache generation
        │
        ▼
Atlas Runtime Physics
  - PhysicsWorld / PhysicsScene
  - PhysicsQuery
  - PhysicsBody / PhysicsShape handles
  - Watcher / Tracy / DebugDraw
        │
        ▼
Backend Adapter
  - JoltPhysicsBackend
  - NullPhysicsBackend
  - TestPhysicsBackend
        │
        ▼
Gameplay Integration
  - CharacterMotor / ServerKCC
  - SkillQuery
  - ProjectileQuery
  - TriggerSystem
  - Vision / LOS Query
```

Jolt 位于 backend adapter 内部。Atlas 的长期契约位于 `src/lib/physics`，Jolt 实现位于独立库，例如 `src/lib/physics_jolt`。

---

## 4. 分层职责

### 4.1 Authoring 层

Unity 2022+ 负责关卡与碰撞内容生产：

- 标记服务端碰撞体、触发器、技能阻挡、投射物阻挡、不可行走面。
- 导出静态碰撞、地形、体积区域、材质、layer、flags。
- 在 Editor 中显示即将导出的 server collision preview。
- 在导出前校验非法 transform、不可读 mesh、未映射 layer、过大 mesh 等问题。

Unity 不决定 Atlas runtime 结构，也不直接生成唯一 Jolt 运行时资产。

### 4.2 Asset 层

Atlas collision asset 是长期稳定边界。它应包含：

| 字段 | 用途 |
|---|---|
| `version` | Atlas 碰撞资源格式版本 |
| `coordinate_system` | 坐标系、单位、轴约定 |
| `source_hash` | Unity 场景或导出源 hash |
| `objects` | shape、transform、layer、material、flags |
| `mesh_buffers` | 静态 mesh 顶点/索引数据 |
| `heightfields` | Terrain height samples、holes、material ids |
| `volumes` | gameplay volume 数据 |
| `chunks` | 大地图分块信息 |

Jolt cache 只记录后端派生数据：

```text
collision asset + Jolt version + backend config -> joltcache
```

cache 可删除、可重建、可因版本不匹配失效。

### 4.3 Cooking 层

`atlas_tool` 负责校验和 cooking：

```text
atlas_tool validate_collision map.collision
atlas_tool cook_collision map.collision --backend jolt
atlas_tool dump_collision map.collision --obj map_collision.obj
```

Cooking 职责：

- 删除退化三角形和非法数据。
- 修正或拒绝错误 winding、NaN、Inf、零面积 primitive。
- 将 Unity layer/tag/component 映射到 Atlas 稳定枚举。
- 按 chunk 生成 bounds、统计信息和资源 hash。
- 生成调试预览和后端 cache。

运行时可以在 cache 缺失时加载源资产并构建后端对象，但生产部署应优先使用已 cook 资产。

### 4.4 Runtime 服务层

Runtime 服务层提供 Atlas 类型的 API：

```cpp
namespace atlas::physics {

struct SceneHandle;
struct BodyHandle;
struct ShapeHandle;
struct MaterialId;

class PhysicsBackend {
 public:
  virtual Result<SceneHandle, Error> CreateScene(const SceneDesc& desc) = 0;
  virtual void DestroyScene(SceneHandle scene) = 0;

  virtual Result<BodyHandle, Error> CreateBody(
      SceneHandle scene,
      const BodyDesc& desc) = 0;
  virtual void DestroyBody(SceneHandle scene, BodyHandle body) = 0;

  virtual bool Raycast(
      SceneHandle scene,
      const RaycastQuery& query,
      RaycastHit* hit) const = 0;

  virtual bool ShapeCast(
      SceneHandle scene,
      const ShapeCastQuery& query,
      ShapeCastHit* hit) const = 0;

  virtual int Overlap(
      SceneHandle scene,
      const OverlapQuery& query,
      std::span<OverlapHit> hits) const = 0;
};

}
```

公开 API 使用 Atlas math、Atlas handles、Atlas layer/mask/filter，不使用 Jolt 类型。

### 4.5 Gameplay 集成层

Gameplay 系统使用 `PhysicsQuery`，不直接访问 backend：

| 系统 | 物理能力 |
|---|---|
| CharacterMotor / ServerKCC | capsule shape cast、ground probe、depenetration、step、slope、snap |
| SkillQuery | sphere/box/capsule overlap、shape cast、LOS check、命中过滤 |
| ProjectileSystem | raycast、sphere cast、pierce/ricochet query |
| TriggerSystem | overlap、sensor volume、enter/leave 状态 |
| AI / Navigation | ground/LOS/nav blocker query，不以物理替代寻路 |
| Visibility / Anti-cheat | line of sight、阻挡验证、异常移动审计 |

角色、怪物和投射物默认是 Atlas 驱动的 kinematic 对象。动态刚体是扩展能力，不是基础玩法模型。

---

## 5. Jolt 后端策略

### 5.1 推荐使用

Jolt backend 第一阶段使用以下能力：

- Static body / kinematic body
- Box、sphere、capsule、convex hull、mesh、heightfield
- Raycast、shape cast、overlap
- Broadphase layer 与 object layer
- Sensor body / trigger query
- Debug draw 数据导出
- 后端统计信息

### 5.2 暂不作为权威玩法基础

以下能力可以保留扩展点，但不作为早期权威玩法基础：

- 大规模 dynamic rigidbody simulation
- constraint / joint gameplay
- ragdoll authority
- vehicle authority
- destruction authority
- 依赖 contact callback 的技能命中

这些能力的结果更难预算、回放、跨 Cell 迁移和长期平衡。它们可以用于局部场景或表现辅助，但不能替代 Atlas 的移动、战斗和实体调度规则。

### 5.3 封装红线

- `JPH::*` 类型不出 `physics_jolt` 边界。
- `BodyHandle` 不等于 `JPH::BodyID`，只能由 adapter 映射。
- Jolt 内存、线程、job 配置由 Atlas config 控制。
- Jolt callback 不直接执行业务逻辑，只写入受控事件队列或统计。
- Jolt 版本升级不能要求 gameplay 代码改动。

---

## 6. Scene、Space 与 Cell

### 6.1 概念关系

```text
CellApp
  └── Space
        ├── EntitySet
        ├── AOI
        ├── PhysicsScene
        ├── NavigationScene
        └── ScriptContext
```

`PhysicsScene` 生命周期跟 `Space` 绑定。副本、竞技场、开放世界区域各自拥有独立 scene 或 scene 分区，查询不得跨 Space。

### 6.2 第一版部署

第一版可以采用：

```text
一个 Space 一个 PhysicsScene
静态地图一次性加载
动态 body 数量受限
```

这能先验证移动、技能、投射物和 Unity 导出管线。

### 6.3 长期分区

接口必须预留：

```text
Space
  └── PhysicsScene
        ├── PhysicsChunk 0
        ├── PhysicsChunk 1
        └── PhysicsChunk N
```

用于：

- 大地图流式加载/卸载。
- Cell 边界附近重复加载只读静态 chunk。
- 查询限制在 Cell 本地和 border 区域。
- 战斗锁定期间禁止实体跨 Cell 迁移。
- 跨 Cell 投射物/技能使用明确的代理或转发策略。

不要把 API 写死成不可拆分的全图 Jolt scene。

---

## 7. Tick 集成

物理系统必须在固定 tick 中运行，不允许系统随意在任意线程修改 runtime body。

推荐 CellApp tick 顺序：

```text
1. 接收网络输入和脚本命令
2. 脚本 pre-tick，生成移动/技能意图
3. 同步动态 PhysicsProxy transform 到 PhysicsScene
4. MovementSystem 执行 KCC、dash、knockback
5. ProjectileSystem 执行 raycast/shape cast
6. SkillSystem 执行 overlap/shape cast/LOS
7. TriggerSystem 评估 enter/leave/stay
8. 提交 Transform 权威结果
9. AOI / snapshot / combat event 广播
10. Watcher / Tracy / debug draw flush
```

如果启用 Jolt simulation step，它只能出现在明确阶段，例如第 3 与第 4 步之间，并受配置开关和预算控制。早期可以只使用 query backend，不跑自由动态模拟。

### 7.1 并发与预算

物理后端不能绕过 Atlas 的进程级资源控制。Jolt 的 job/thread、临时分配器、body 上限、contact pair 上限和 batch query 上限必须来自 `ServerConfig` 或 Space 配置。

建议配置项：

```text
physics.max_threads
physics.temp_allocator_bytes
physics.max_bodies_per_scene
physics.max_dynamic_bodies_per_scene
physics.max_contact_pairs
physics.max_queries_per_tick
physics.max_script_queries_per_tick
physics.slow_query_us
```

CellApp 默认仍以 cell 内串行提交权威状态为模型。后台物理 job 只能处理只读查询批次或 backend 内部工作，不能并发修改 Entity、Transform、Script 状态。

---

## 8. Collision Layer 与过滤

Layer 设计必须稳定、可审计、可工具化。Unity layer 编号不能直接进入服务端资源，必须映射为 Atlas 枚举。

建议基础 object layer：

| Layer | 用途 |
|---|---|
| `StaticWorld` | 地面、墙体、建筑、不可穿越大障碍 |
| `StaticNoWalk` | 不可站立但可能阻挡的区域 |
| `DynamicObstacle` | 门、移动平台、临时阻挡 |
| `Character` | 玩家角色 collider |
| `Monster` | 怪物 collider |
| `Npc` | 非战斗 NPC collider |
| `Projectile` | 投射物查询/传感器 |
| `SkillHitbox` | 技能体积查询 |
| `Trigger` | gameplay trigger |
| `VisionBlocker` | 视线遮挡 |
| `ProjectileBlocker` | 投射物遮挡 |
| `Water` | 水体区域 |
| `Ladder` | 攀爬区域 |
| `NavBlocker` | 导航阻挡 |

每个 query 必须携带：

```text
object layer
query mask
flags
result limit
optional custom filter
```

示例：

| Query | Include | Custom filter |
|---|---|---|
| 角色移动 | `StaticWorld | DynamicObstacle` | slope、material、one-way platform |
| 地面检测 | `StaticWorld | DynamicObstacle` | walkable、max slope |
| 投射物 | `StaticWorld | ProjectileBlocker | Character | Monster` | owner、faction、pierce |
| 技能命中 | `Character | Monster` | team、iframe、super armor、hit-once |
| 视线 | `StaticWorld | VisionBlocker` | material、transparent flags |
| Trigger | `Character | Monster` | trigger kind、quest/session state |

---

## 9. Entity 与 PhysicsProxy

Entity 不是 PhysicsBody。物理对象是实体的可选代理。

```text
EntityId
  ├── TransformComponent
  ├── MovementComponent
  ├── CombatComponent
  └── PhysicsProxyComponent
```

`PhysicsProxyComponent` 保存 Atlas handle：

```cpp
struct PhysicsProxyComponent {
  physics::BodyHandle body;
  physics::ShapeHandle shape;
  physics::ObjectLayer layer;
  physics::BodyFlags flags;
};
```

运行时维护双向映射：

```text
EntityId -> BodyHandle
BodyHandle -> EntityId
```

Query hit 返回 `BodyHandle` 和可选 `EntityId`。上层过滤使用 Entity 状态，但不能在 backend callback 中直接访问实体系统。

---

## 10. Unity 碰撞导出

### 10.1 Authoring 组件

Unity 侧使用显式 authoring 组件，而不是默认导出所有 collider：

```csharp
public sealed class ServerColliderAuthoring : MonoBehaviour
{
    public ServerCollisionKind kind;
    public ServerCollisionFlags flags;
    public string material;
    public bool exportToServer = true;
}
```

常见 Collider 映射：

| Unity | Atlas / Jolt |
|---|---|
| `BoxCollider` | box shape |
| `SphereCollider` | sphere shape |
| `CapsuleCollider` | capsule shape |
| `MeshCollider` convex=false | static mesh shape |
| `MeshCollider` convex=true | convex hull shape |
| `TerrainCollider` | heightfield 或 mesh tiles |
| trigger collider | Atlas volume 或 sensor body |

Primitive 应保持 primitive，不应默认烘成 mesh。

### 10.2 Transform 约束

项目规则：

- 单位为 meter。
- 坐标约定为 X right、Y up、Z forward。
- Quaternion 导出字段顺序明确为 x、y、z、w。
- Sphere/Capsule 不允许非均匀缩放。
- 服务端 primitive 不允许负缩放。
- 非法缩放需要导出报错，不能静默修正。

### 10.3 Terrain

长期使用 heightfield。第一版若需要快速验证，可以导出 mesh tiles，但大型开放世界必须回到 heightfield/chunked terrain。Terrain 资产需要包含：

- origin、size、resolution
- height samples
- hole mask
- material ids
- chunk bounds

### 10.4 Gameplay Volume

不是所有 trigger 都需要进入 Jolt。安全区、水体、死亡区、任务区域、副本边界等可以作为 Atlas volume 由 Space/TriggerSystem 管理。只有需要 raycast、overlap 或 sensor 接触的 volume 才注册到 PhysicsScene。

---

## 11. CharacterMotor 与移动

Atlas 自己实现服务端 CharacterMotor。Jolt 只提供查询事实：

```text
capsule shape cast
capsule overlap
ground probe
raycast
```

CharacterMotor 控制：

- 输入到速度的规则。
- dash、roll、knockback、airborne、root-motion curve 的位移解释。
- slope limit、step up、snap to ground。
- depenetration 限制。
- 可穿越对象、技能期间碰撞 mask、霸体状态。
- 移动审计和反作弊。

这样可以确保移动系统服务于 Atlas 的网络、脚本和战斗模型，而不是被物理库的 CharacterController 约束。

---

## 12. 技能、投射物与延迟补偿

物理查询为战斗提供候选和阻挡信息，但战斗规则仍由 Combat 系统裁决。

技能命中流程：

```text
Skill timeline action
  -> 构造 query shape
  -> Physics overlap/shape cast 获取候选
  -> Combat filter：阵营、状态、iframe、hit-once、super armor
  -> 可选 LOS / blocker query
  -> 生成 combat event
```

PvP lag compensation 需要历史 Transform / collider 状态。物理 backend 不应直接回滚整个 Jolt scene。推荐方式：

- 对角色/怪物保存 1 秒历史胶囊或 hit primitive。
- lag compensation 使用历史 primitive 做命中过滤。
- 静态世界阻挡使用当前 PhysicsScene。
- 动态障碍若影响 PvP 公平，需要保存简化历史状态或禁止进入高公平场景。

这比回滚完整物理世界更可控。

---

## 13. 脚本 API 边界

C# 脚本可以使用受控查询：

```csharp
Physics.Raycast(...)
Physics.OverlapSphere(...)
Physics.OverlapBox(...)
Physics.ShapeCast(...)
```

约束：

- 必须指定 layer/mask。
- 必须提供结果上限或使用预分配结果缓冲。
- 单 tick 查询预算可配置。
- 返回 EntityId、hit point、normal、material，不返回后端对象。
- 脚本不能直接创建无限 collider；动态 collider 创建走 Entity/Component 生命周期。

---

## 14. 可观测性

Watcher 指标：

```text
physics.scenes
physics.bodies.static
physics.bodies.dynamic
physics.shapes
physics.memory.bytes
physics.queries.raycast.count
physics.queries.shape_cast.count
physics.queries.overlap.count
physics.queries.time_us.total
physics.queries.time_us.p95
physics.step.time_us
physics.sync.time_us
physics.cache.hit
physics.cache.miss
physics.debug.slow_query.count
```

Tracy zones：

```text
Physics.LoadScene
Physics.CookCacheLoad
Physics.SyncBodies
Physics.Step
Physics.Raycast
Physics.ShapeCast
Physics.Overlap
Physics.TriggerEvaluate
Movement.CharacterMotor
Skill.PhysicsQuery
Projectile.PhysicsQuery
```

慢查询需要记录：

- SpaceId / CellId
- query kind
- layer/mask
- shape type
- result count
- elapsed us
- caller system

生产环境不记录完整坐标明细，除非开启诊断采样。

---

## 15. 测试策略

### 15.1 Backend 无关测试

使用 TestPhysicsBackend 覆盖：

- CharacterMotor 状态机。
- 技能过滤逻辑。
- query budget。
- layer/mask 组合。
- 脚本 API 边界。

### 15.2 Jolt 集成测试

覆盖：

- primitive raycast/overlap/shape cast。
- mesh 和 heightfield query。
- layer filter。
- BodyHandle 与 EntityId 映射。
- scene load/unload。
- cache version mismatch。

### 15.3 内容管线测试

CI 检查：

- Unity exporter 生成的 collision asset 可 validate。
- cooking 输出 deterministic hash。
- preview mesh 可生成。
- 非法 transform 会失败。
- layer/material 映射完整。

---

## 16. 版本与升级

版本边界：

```text
Atlas collision asset version != Jolt version
Atlas physics API version != backend cache version
```

Runtime 加载时检查：

- collision asset version 是否支持。
- backend cache version 是否匹配当前 backend。
- source hash 是否匹配。
- layer/material table 是否匹配当前项目配置。

Jolt 升级流程：

```text
1. 升级 physics_jolt。
2. 跑 backend 集成测试。
3. 标记旧 joltcache 失效。
4. 重新 cook 官方测试地图。
5. 对比 query golden results。
6. 进入项目地图批量 recook。
```

---

## 17. 实施顺序

1. 建立 `src/lib/physics` API、handles、query types、layer/mask、Null/Test backend。
2. 建立 `src/lib/physics_jolt`，只实现 static scene、primitive、mesh、raycast、shape cast、overlap。
3. 建立 `docs/physics` 和 `atlas_tool` collision validate/cook/dump 命令。
4. 建立 Unity exporter MVP：primitive、static mesh、layer、material、preview。
5. 在 CellApp Space 中挂接 PhysicsScene 生命周期。
6. 实现 CharacterMotor 查询接入。
7. 实现 SkillQuery、ProjectileQuery、TriggerSystem 接入。
8. 引入 heightfield、chunk、cache、slow-query diagnostics。
9. 设计跨 Cell physics chunk 和 border query 策略。

实施顺序可以按项目节奏调整，但架构红线不能降低。

---

## 18. 架构红线

1. Gameplay/server/script 不 include Jolt 头。
2. Atlas collision asset 不等于 Jolt cooked binary。
3. Entity 不等于 PhysicsBody。
4. 角色移动不交给 Jolt CharacterController。
5. 技能命中不直接依赖动态刚体 contact callback。
6. 所有 query 必须有 layer/mask/filter/result limit。
7. PhysicsScene 生命周期必须跟 Space/Cell 生命周期明确绑定。
8. 物理资源必须 validate/cook/debug preview。
9. 物理性能必须进入 Watcher 和 Tracy。
10. Null/Test backend 必须长期保留。

---

## 19. 需要同步更新的旧文档

现有 gameplay 文档中部分早期假设需要在物理子系统设计确认后更新：

- `MOVEMENT_SYNC.md` 中“不做完整物理模拟 / 高度图足够”的描述应改为“移动核心由 Atlas CharacterMotor 控制，底层可使用 Atlas PhysicsQuery/Jolt 查询”。
- `HIT_VALIDATION.md` 中“不做物理 raycast / 自研形状扫描”的描述应改为“命中过滤和 lag compensation 由 Combat 系统控制，候选查询和阻挡可使用 Atlas PhysicsQuery”。
- `OVERVIEW.md` 中关于 C++ 共享移动仿真的表述需要区分纯规则计算与服务端物理查询依赖。

这些文档不应在物理架构未评审前零散改动，避免下游设计反复。
