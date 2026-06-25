# 服务端导航子系统设计

> **用途**：定义 Atlas 服务端寻路（navmesh / pathfinding）子系统的分层、资源管线、
> 后端隔离与 v1 边界。与 `docs/physics/physics_architecture.md` 同构：Atlas 拥有
> 导航架构，Recast/Detour 是首个生产级后端。
>
> **状态**：✅ 当前 v1 基线。契约层、Recast 后端、atlas_tool 命令、Space 集成与
> MoveTo 入口已落地；§6 列出仍在 v1 之外的边界。
>
> **范围**：本文描述当前 v1 设计与后续边界。

---

## 1. 核心决策

```text
Atlas 拥有导航架构；Recast/Detour 是首个生产级后端。
```

上层（server / cellapp / script / AI）只依赖 `atlas::nav` 的 API、`NavParams` 资源
格式和 `NavQuery`，不 include Recast/Detour 头、不存 `dtPolyRef`——与物理子系统
“Jolt 类型不出 `physics_jolt` 边界”同一条红线。物理给碰撞，导航给寻路，物理不替代寻路。

## 2. 分层

```text
src/lib/navigation/          契约层，零第三方
  nav_query.h/.cc            NavQuery 接口 + NullNavQuery + 路径/点/过滤类型
  nav_params.h/.cc           NavParams（烘焙参数 + 包围盒 + layer 角色 + override），不含几何
  nav_input.h/.cc            DeriveNavInput：collision 几何 → Recast 三角输入
  nav_backend.h              NavBackendFactory 抽象（后端注入点）
src/lib/navigation_recast/   后端，唯一 include Recast/Detour 的地方（ATLAS_ENABLE_RECAST）
```

几何**不在 nav 资源里复制**：`cook_nav` 的输入是 collision 资产（`.collision.json` + `.bin`）
加一份 `NavParams` sidecar，三角几何只在内存里由 `DeriveNavInput` 派生。

## 3. 几何派生（`DeriveNavInput`）

派生层只做**包含过滤 + 三角化 + area 标注**；**可行走与否交给 Recast**
（按体素坡度 + agent 净空判定），派生层不替 Recast 做可走判断。

- **layer 角色表**（`NavParams.layer_roles`，默认全 `kInclude`）：把 collision layer
  映射到 `include` / `carve` / `ignore`。数据驱动，不硬编码尚未固化的 layer 枚举。
- **图元三角化**：`box`→12 三角；`mesh`→透传（顶点已是世界空间）；`heightfield`→
  网格三角，世界 `y = origin.y + scale.y·sample`，触到 `FLT_MAX`（hole）样本的 cell 整体跳过。
- **v1 跳过**：`convex`（点云需凸包三角化）、`sphere`、`capsule`、`plane`（无限面需包围盒裁剪）
  导出时计数 + warn，不致空跑。
- **override volumes**（世界空间 AABB，空间定义、对 collision 重导出鲁棒）：
  `area_tag` 按三角质心改 area；`force_walkable` / `force_blocker` 追加 AABB 几何。
- **包围盒**：显式给定则直接用之（不加 margin），否则取派生几何 AABB 外扩 `margin`。
- **缠绕**：v1 保留源缠绕，提供 `flip_winding` 逃生阀；“烘出空 navmesh”的硬校验
  属于 bake 阶段（见 §6）。
- **调参风险**：Recast 按 agent 半径腐蚀可行走区，比 2×半径还窄的门洞 / 栈道会被吃掉，
  `cell_size` 偏大会加剧；由 `path_nav` 验通道连通性 + `cook_nav` 的可走面积兜底（见 §5）。

## 4. 查询契约（`NavQuery`）

`FindPath` / `NearestPoint` / `Raycast`。要点：

- **三态路径** `NavPathStatus`：`kReached` / `kPartial`（到最近可达点）/ `kEmpty`。
- **过滤**用 Detour area 模型（`area_cost[64]` + include/exclude flags），区别于物理
  `LayerMask`——导航 area 是行走代价类，不是碰撞分组。
- **线程模型**：单实例**非并发**（后端持可变搜索 node 池）；并发寻路按线程池化 query。
  `max_search_nodes` 是预算，超出返回部分路径。
- **`NullNavQuery`**：无 navmesh 时一律返回 `kEmpty` / off-mesh，**绝不伪造直线路径**，
  让移动 / AI 测试可在无 Recast 后端时运行。

## 5. 后端与工具

- `navigation_recast`：Recast 离线烘焙 + Detour 运行时查询；`dt*`/`rc*` 不出该库；
  `ATLAS_ENABLE_RECAST` 门控，FetchContent 接入照 Jolt（关 demo/test、匹配 /MD 运行库、
  版本 pin）；`tools/bin/check_recast_isolation.{bat,sh}` 守 CI。
- `atlas_tool`：`validate_nav`（纯资产层，无 recast 也可用）；`cook_nav` 内存 bake 并打印
  统计（poly / 顶点 / 可走面积 / skip 数，v1 不落 `.navcache`）；`dump_nav --obj` 导可走面
  OBJ；`path_nav --from --to [--obj]` 烘焙 + 单次 FindPath，打印状态 / 长度并可导路径折线
  （不依赖 Unity/CellApp 的独立可视化）。后三者 `ATLAS_ATLAS_TOOL_HAS_RECAST` 门控。
- **v1 直接内存 bake**：不落 `.navcache`、不做 stamp/recook；Detour tile 的裸序列化
  跨平台问题随之推迟到有持久化需求时再处理。
- **Space 集成**：`Space` 默认持 `NullNavQuery`；CellApp Init 注入
  `RecastNavBackendFactory`（`ATLAS_CELLAPP_HAS_RECAST`）并由 `MakeSpace` 继承；
  `Space::LoadNavMeshFromFiles(collision, params)` 在线内存 bake 后替换
  `Space::NavQuery()`，无 backend 时拒绝而非静默留 Null。C# 脚本经
  `CellServerEntity.LoadNavMesh(spaceId, collisionPath, paramsPath)` 走同一入口。
- **MoveTo**：`AddNavMoveController` 在入口处用 `Space::NavQuery()` 规划一次，
  把路点交给 `MoveAlongPathController` 沿线走——controller 不持 NavQuery 指针
  （navmesh 重载不悬挂），路点随 offload 迁移序列化；部分路径走到最近可达点，
  到达由脚本按位置判断。C# 入口 `CellServerEntity.NavMoveTo(destination, speed)`。

## 6. 非目标（v1 不做）

v1 不做以下，留接口不留实现：

- 寻路 LOD / tick 预算、路径缓存与切片 A*（v1 的 MoveTo 在入口同步规划一次）。
- 动态重规划（目标移动 / 撞墙后自动重路由）：v1 由脚本重新发起 MoveTo。
- off-mesh 连接（跳台 / 落差 / 高地路线）：`NavParams` 不带 links，bake 纯地面。
- tiled / chunk navmesh、`.navcache` 持久化、Watcher/Tracy 指标。
- 动态障碍（`dtTileCache`）、多 agent 半径剖面。
- 精确“最窄通道宽度”报告（medial-axis 分析）：后续；v1 用 `path_nav` 连通性 + `cook_nav`
  可走面积兜底（“烘出空 navmesh”硬校验已随 bake 落地）。

## 7. 与 BigWorld 的偏离

保留 BigWorld 范式：离线烘焙、navmesh 为派生资产、服务端权威寻路。偏离点：用
Recast/Detour 取代 BigWorld 自研 girth navgen（业界标准、开源、接入干净，与“Jolt 作
首个物理后端”同一判断）；多 girth（多 agent 半径）→ 多剖面，后置。
