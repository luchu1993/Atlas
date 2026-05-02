# Atlas Engine — 引擎开发路线图

C# 脚本层架构(`atlas_engine` 共享库 + .NET 9 CoreCLR + Source
Generator + 热重载)已落地,详见
[`docs/scripting/`](../scripting/);本文档负责脚本层之上的引擎能力分阶段
规划。每个 Phase 独立文档承载该阶段的设计决策、当前状态与剩余工作。

## 阶段总览

| Phase | 名称 | 状态 | 关键交付 | 文档 |
|---|---|---|---|---|
| 5 | 服务器框架基类 | ✅ | ServerApp 主循环、消息接口注册、Watcher | [phase05](phase05_server_framework.md) |
| 6 | machined 进程管理 | ✅ | 服务发现、进程注册、心跳监控、atlas_tool watch/shutdown | [phase06](phase06_machined.md) |
| 7 | DBApp + 数据库层 | 🚧 SQLite 默认 / MySQL ⬜ | IDatabase / SQLite/XML 后端 / DB watcher | [phase07](phase07_dbapp.md) |
| 8 | BaseApp 实体宿主 | ✅ | Base/Proxy 实体、客户端代理、WriteToDB | [phase08](phase08_baseapp.md) |
| 9 | LoginApp + BaseAppMgr | ✅ 主线 / 🚧 churn 收敛 | 登录流程、SessionKey、负载分配 | [phase09](phase09_login_flow.md) |
| 10 | CellApp 空间模拟 | ✅ | Space、RangeList、AOI/Witness、Controller；持续优化在 docs/optimization | [phase10](phase10_cellapp.md) |
| 11 | 分布式空间（Real/Ghost + CellAppMgr） | ✅ 主线 | Ghost 机制、Entity Offload、BSP 分区、CellAppMgr 负载均衡 | [phase11](phase11_distributed_space.md) |
| 12 | 客户端 SDK | ✅ 主线 / ⬜ 高层 async API | atlas_net_client、Atlas.Client、AvatarFilter、Unity 包骨架 | [phase12](phase12_client_sdk.md) |
| 13 | 高可用（Reviver + DBAppMgr） | ⬜ | 崩溃恢复、Manager 热备、集群管理 | [phase13](phase13_high_availability.md) |

## 依赖关系

```
脚本层架构(docs/scripting/)
        │
        ▼
   Phase 5  服务器框架
        │
        ▼
   Phase 6  machined ────────────────┐
        │                             │
        ▼                             │
   Phase 7  DBApp + DB 层            │
        │                             │
        ▼                             │
   Phase 8  BaseApp ◄─────────────────┘
        │
        ├──────────┐
        ▼          ▼
   Phase 9    Phase 10
   LoginApp + CellApp
   BaseAppMgr     │
        │         ▼
        │    Phase 11
        │    Real/Ghost + CellAppMgr
        │         │
        ▼         ▼
   Phase 12 客户端 SDK
        │
        ▼
   Phase 13 高可用
```

## 里程碑

| 里程碑 | 覆盖阶段 | 验收标准 |
|---|---|---|
| **M-Single** | Phase 5–8 | 单 BaseApp + 单 DBApp 可创建 / 持久化实体 |
| **M-Login** | + Phase 9 | 客户端可登录并创建角色 |
| **M-World** | + Phase 10 | 角色在空间中移动，AOI 可工作 |
| **M-Distributed** | + Phase 11 | 多 CellApp 负载均衡，跨 Cell 无缝迁移 |
| **M-Client** | + Phase 12 | Unity 客户端可接入完整流程 |
| **M-Production** | + Phase 13 | 具备崩溃恢复和故障转移能力 |
