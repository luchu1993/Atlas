# Phase 7: DBApp + 数据库层

> 前置依赖: Phase 5 (ServerApp/ManagerApp), Phase 6 (machined), Script Phase 4 最小持久化元数据子集
> BigWorld 参考: `server/dbapp/`, `lib/db_storage/idatabase.hpp`, `lib/db_storage_mysql/`, `lib/db_storage_xml/`
> 文档状态: 2026-04-18 当前代码对齐版

---

## 目标

Phase 7 的目标是把数据库层收敛成一个**可持续演进的单 DBApp 架构**:

- `DBApp` 作为唯一数据库代理进程，隔离 `BaseApp` / `LoginApp` 与底层存储实现
- `IDatabase` 提供统一后端接口，屏蔽 XML / SQLite / MySQL 差异
- 保证实体 `CRUD`、`lookup_by_name`、`checkout / clear_checkout`、`auto_load` 语义稳定
- 支撑当前登录主链路和后续 `checkin / checkout / relogin rollback` 场景

---

## 总体状态

| 子阶段 | 主题 | 状态 |
| --- | --- | --- |
| P7.1 | 基线收敛（文档与默认策略对齐） | ✅ |
| P7.2 | SQLite backend 落地并切为默认后端 | ✅ |
| P7.3 | DBApp 集成补强（登录回滚 / BaseApp 故障） | 🚧 |
| P7.4 | MySQL backend 进入正式实现 | ⬜ |

当前结论:

- **DBApp 主体已经落地** ✅
- **XML backend 已经可用** ✅
- **SQLite backend 已经落地并切成开发默认后端** ✅
- **下一步主线是继续补强 DBApp 集成测试矩阵与故障路径验证** 🚧
- **MySQL 继续作为正式后端预选推进** ⬜

---

## 验收状态

- [x] `IDatabase` 接口已落地，包含 `put/get/del/lookup/checkout/clear_checkout/auto_load`
- [x] `DBApp` 进程已落地，可启动、注册到 `machined`、处理数据库消息
- [x] `XmlDatabase` 已落地，并具备延迟回调与缓冲 flush 能力
- [x] `CheckoutManager` 已落地，支持 `Checking / Confirmed` 两阶段内存占位
- [x] `DBApp` 已支持 `AbortCheckout`，用于登录回滚场景
- [x] `DBApp` 已能处理 `LoginApp -> DBApp` 的 `AuthLogin`
- [x] `EntityDefRegistry` 已支持 `DBApp` 从 `entity_defs.json` 加载
- [x] 单元测试已覆盖 `dbapp_messages`、`checkout_manager`、`xml_database`、`sqlite_database`、`database_factory` 与 `server_config` 的数据库配置分支
- [x] `SQLite` backend 已实现，接入 `database_factory` / `ServerConfig` / `DBApp`
- [x] 当前代码默认 `db_type` 已切换为 `sqlite`
- [x] 已补充 `DBApp` 基础集成测试（`test_dbapp_login_flow`、`test_dbapp_checkout_cleanup`、`test_login_flow`）
- [ ] `MySQL` backend 尚未作为当前主线完成落地（`src/lib/db_mysql/` 仍为占位 CMake 骨架）
- [~] `AbortCheckout`、更完整登录链路矩阵、长稳运行与更多故障路径回归仍需继续补强
- [ ] 仍未进入多 `DBApp` / `DBAppMgr` 分片阶段

---

## 当前代码现实 ✅（P7.1 已完成）

仓库中与 Phase 7 直接相关的已落地模块:

- `src/lib/db/idatabase.h`
- `src/lib/db/database_factory.cc`
- `src/lib/db_xml/xml_database.{h,cc}`
- `src/lib/db_sqlite/sqlite_database.{h,cc}`
- `src/lib/db_mysql/CMakeLists.txt`（占位，未实现）
- `src/server/dbapp/dbapp.{h,cc}`
- `src/server/dbapp/dbapp_messages.h`
- `src/server/dbapp/checkout_manager.{h,cc}`
- `src/server/dbapp/entity_id_allocator.{h,cc}`

DBApp 已承担: 加载 `EntityDefRegistry`、按 `ServerConfig.db_type` 创建后端、处理 `WriteEntity / CheckoutEntity / CheckinEntity / DeleteEntity / LookupEntity / AbortCheckout / AuthLogin`、监听 BaseApp 死亡并清理 checkout、在 `on_tick_complete()` pump `database_->process_results()`。

---

## 后端策略 ✅

当前后端优先顺序: **`sqlite`（开发默认）→ `xml`（最小 fallback / smoke test）→ `mysql`（正式部署预选，未实现）**。`MongoDB` 不在主线范围。

数据模型统一采用 `blob + 少量索引列 + checkout 状态列`（`dbid / type_id / identifier / password_hash / blob / auto_load / checked_out / checkout_addr / checkout_app_id / checkout_eid`）。XML 后端作为语义兼容层用文件系统映射这些字段。

---

## 当前接口与协议边界 ✅

### `IDatabase` 主接口（稳定）

`put_entity / get_entity / del_entity / lookup_by_name / checkout_entity / checkout_entity_by_name / clear_checkout / clear_checkouts_for_address / mark_checkout_cleared / get_auto_load_entities / set_auto_load / set_deferred_mode / process_results`。回调统一在主线程 `process_results()` 中交付。

### DBApp 消息面（稳定，Phase 9 登录链路已依赖）

`WriteEntity[Ack] / CheckoutEntity[Ack] / CheckinEntity / DeleteEntity[Ack] / LookupEntity[Ack] / AbortCheckout[Ack] / AuthLogin`。

### 登录相关边界

`DBApp` 只负责账号查找 / 认证数据读取 / 账号自创建；不负责客户端连接、`BaseApp` 分配或重复登录协商。

---

## 工业级实施原则 ✅

1. **统一异步契约**：结果一律通过 `process_results()` 回到主线程，不允许后端行为分裂。
2. **Checkout 必须有 owner 语义**：表达 owner / `Checking` / `Confirmed` / BaseApp 死亡回收。
3. **Fail-safe 清理**：覆盖 `AbortCheckout`、`WriteFlags::LogOff`、`CheckinEntity`、`BaseApp` 死亡、`DBApp` 关闭。
4. **后端切换不影响上层协议**。

---

## 实施计划

### P7.1 基线收敛 ✅

文档、配置、默认策略已与仓库真实状态对齐，`sqlite/xml/mysql` 三类后端定位清晰。

### P7.2 SQLite backend 落地 ✅

已完成 `src/lib/db_sqlite/`、`DatabaseConfig` / `ServerConfig` 扩展、`database_factory.cc` 更新、`DBApp` 接入与单元测试。当前代码默认后端已切换为 `sqlite`，登录主链路可在 `sqlite` 后端跑通，checkout 占用与释放语义稳定。

### P7.3 DBApp 集成补强 🚧

目标:

- 让 `DBApp` 在登录回滚和 BaseApp 故障路径下更稳

已完成:

- `AuthLogin` 基线路径与 `BaseApp death -> clear_checkouts_for_address` 集成验证
- 仓库中已加入 `test_dbapp_login_flow`、`test_dbapp_checkout_cleanup`、`test_login_flow`

仍需补强:

- `AbortCheckout` 与 delayed callback 交叉时的 suppress / 幂等行为
- `CheckinEntity`、`WriteFlags::LogOff`、`auto_load` 在 SQLite 路径下的更多回归
- `test_login_flow` 的长稳运行、短线重登 churn 与故障注入
- `DBApp` watcher / metrics 的使用说明
- `xml / sqlite` 两类后端（以及后续 `mysql`）的契约一致性验证

完成标准:

- `CheckoutEntity -> AbortCheckout`
- `CheckoutEntity -> CheckinEntity`
- `BaseApp death -> clear_checkouts_for_address`
- `AuthLogin` 基线路径

都具备稳定回归测试。

### P7.4 MySQL backend 进入正式实现 ⬜

目标:

- 让 `mysql` 成为正式部署预选

当前状态: `src/lib/db_mysql/CMakeLists.txt` 仅保留占位骨架，`ATLAS_DB_MYSQL` 选项默认 OFF。

范围:

- 实现 `src/lib/db_mysql/` 完整源码
- 建表、索引、唯一约束、事务实现
- 对齐 `sqlite` 的数据模型和语义

完成标准:

- 消息面与 `sqlite/xml` 完全一致
- 行为差异只体现在性能和运维，不体现在协议语义

---

## 配置策略 ✅

`ServerConfig` 已同时支持 `db_xml_dir`、`db_sqlite_path / wal / busy_timeout_ms / foreign_keys`、`db_mysql_*` 系列字段。推荐开发配置使用 `db_type: sqlite`，最小 fallback 使用 `xml`，正式部署预选 `mysql`。

---

## 测试要求

### 已存在测试 ✅

- 单元: `test_database_types`、`test_xml_database`、`test_sqlite_database`、`test_dbapp_messages`、`test_dbapp_entity_id_messages`、`test_checkout_manager`、`test_server_config`
- 集成: `test_dbapp_login_flow`、`test_dbapp_checkout_cleanup`、`test_login_flow`

### 本阶段仍需补强 🚧

- `AbortCheckout` 与 delayed callback 交叉时的 suppress / 幂等行为
- `CheckinEntity`、`WriteFlags::LogOff`、`auto_load` 在 SQLite 路径下的更多回归
- `test_login_flow` 的长稳运行、短线重登 churn 与故障注入
- `xml / sqlite / mysql` 三类后端的契约一致性验证

### 必测场景

至少覆盖: create / update / get / delete、`lookup_by_name`、`password_hash` 的持久化与读回、`checkout_entity`、重复 checkout 返回 owner、`AbortCheckout`、`CheckinEntity`、`WriteFlags::LogOff`、`BaseApp` 死亡后的批量 checkout 清理、`auto_load`、`sqlite` 后端下的登录基线路径。

---

## Definition Of Done

Phase 7 只有满足以下条件，才应视为真正完成:

- 单 `DBApp` 架构稳定可运行 ✅
- `IDatabase` 语义不再频繁变动 ✅
- `xml` 继续可用但降级为 fallback ✅
- `sqlite` 成为当前代码默认且可实际使用的开发后端 ✅
- `mysql` 的接口与数据模型路径明确，不再与开发路径冲突 🚧
- `DBApp` 的 checkout / abort / clear 路径有回归测试 🚧
- Phase 9 登录主链路可在 `sqlite` 后端上稳定验证 ✅

---

## 与其他文档的边界

本文档负责: `DBApp` 的角色与边界、数据库后端策略、`IDatabase` 语义、单 DBApp 实施路线、`sqlite/xml/mysql` 的阶段定位。

本文档不负责: 多 `LoginApp` / `LoginAppMgr` 设计、登录入口负载均衡、`BaseApp` 侧重复登录状态机、`CellApp` / space 持久化策略。

相关文档:

- [phase09_login_flow.md](./phase09_login_flow.md)
- [../MULTI_LOGINAPP_DESIGN.md](../MULTI_LOGINAPP_DESIGN.md)

---

## 不进入当前范围

多 `DBApp` / `DBAppMgr` / 分片 / Rendezvous Hash / `MongoDB` 主线支持 / 大规模 schema migration 框架 / 外部认证系统替代。
