# Atlas Engine — 后脚本层开发路线图

C# 脚本层（[`docs/scripting/`](../scripting/) Phase 0–6）完成后的引擎
开发规划。每个 Phase 独立文档承载该阶段的设计决策、当前状态与剩余工作。

## 脚本层基线

| Phase | 名称 | 状态 | 文档 |
|---|---|---|---|
| Script 0 | 清理 Python + 建立抽象层 | ✅ | [script_phase0](../scripting/script_phase0_cleanup_abstraction.md) |
| Script 1 | .NET 9 运行时嵌入 | ✅ | [script_phase1](../scripting/script_phase1_dotnet_host.md) |
| Script 2 | C++ ↔ C# 互操作层 | ✅ | [script_phase2](../scripting/script_phase2_interop_layer.md) |
| Script 3 | Atlas 引擎 C# 绑定 | 🚧 主路径可用 | [script_phase3](../scripting/script_phase3_engine_bindings.md) |
| Script 4 | 共享程序集 + Source Generator | 🚧 服务端依赖子集可用 | [script_phase4](../scripting/script_phase4_shared_generators.md) |
| Script 5 | 热重载机制 | 🚧 基础设施已在库内 | [script_phase5](../scripting/script_phase5_hot_reload.md) |
| Script 6 | 测试与稳定化 | 🚧 测试矩阵已铺开 | [script_phase6](../scripting/script_phase6_testing.md) |

## 后脚本层阶段

| Phase | 名称 | 状态 | 关键交付 | 文档 |
|---|---|---|---|---|
| 5 | 服务器框架基类 | ✅ | ServerApp 主循环、消息接口注册、Watcher | [phase05](phase05_server_framework.md) |
| 6 | machined 进程管理 | ✅ 主线 / 🚧 atlas_tool & 重连 | 服务发现、进程注册、心跳监控 | [phase06](phase06_machined.md) |
| 7 | DBApp + 数据库层 | 🚧 SQLite 默认 / MySQL ⬜ | IDatabase / SQLite/XML 后端 / DB watcher | [phase07](phase07_dbapp.md) |
| 8 | BaseApp 实体宿主 | ✅ | Base/Proxy 实体、客户端代理、WriteToDB | [phase08](phase08_baseapp.md) |
| 9 | LoginApp + BaseAppMgr | ✅ 主线 / 🚧 churn 收敛 | 登录流程、SessionKey、负载分配 | [phase09](phase09_login_flow.md) |
| 10 | CellApp 空间模拟 | ✅ | Space、RangeList、AOI/Witness、Controller；持续优化在 docs/optimization | [phase10](phase10_cellapp.md) |
| 11 | 分布式空间（Real/Ghost + CellAppMgr） | ✅ 主线 | Ghost 机制、Entity Offload、BSP 分区、CellAppMgr 负载均衡 | [phase11](phase11_distributed_space.md) |
| 12 | 客户端 SDK | 🚧 核心骨架 / AvatarFilter ⬜ | 连接协议、实体同步、控制台测试客户端 | [phase12](phase12_client_sdk.md) |
| 13 | 高可用（Reviver + DBAppMgr） | ⬜ | 崩溃恢复、Manager 热备、集群管理 | [phase13](phase13_high_availability.md) |

## 依赖关系

```
脚本层基线 (Script 0–3 + Script 4 最小子集)
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
