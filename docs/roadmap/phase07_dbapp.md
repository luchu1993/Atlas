# Phase 7: DBApp + 数据库层

> 前置依赖: Phase 5 (ServerApp/ManagerApp), Phase 6 (machined), Script Phase 4 最小持久化元数据子集
> BigWorld 参考: `server/dbapp/`, `lib/db_storage/idatabase.hpp`, `lib/db_storage_mysql/`, `lib/db_storage_xml/`
> 文档状态: 2026-04-12 可落地实施版

---

## 目标

Phase 7 的目标是把数据库层收敛成一个**可持续演进的单 DBApp 架构**:

- `DBApp` 作为唯一数据库代理进程，隔离 `BaseApp` / `LoginApp` 与底层存储实现
- `IDatabase` 提供统一后端接口，屏蔽 XML / SQLite / MySQL 差异
- 保证实体 `CRUD`、`lookup_by_name`、`checkout / clear_checkout`、`auto_load` 语义稳定
- 支撑当前登录主链路和后续 `checkin / checkout / relogin rollback` 场景

本文档不再把“已实现现状”和“远期设想”混在一起，而是明确区分:

- **当前已落地**
- **本阶段必须继续落地**
- **后续阶段预留**

---

## 结论

当前 Phase 7 的主线结论如下:

- 当前 Atlas 已经有可运行的 `DBApp`、`IDatabase`、`XmlDatabase`、`CheckoutManager`
- 当前开发阶段数据库后端策略调整为:
  - **`sqlite`：推荐开发默认后端**
  - **`xml`：最轻量 fallback / smoke test 后端**
  - **`mysql`：正式后端预选**
- `MongoDB` 不作为当前主线后端方案
- `DBApp` 继续保持**异步回调契约**:
  - 后端内部可以同步执行
  - 但回调统一通过 `process_results()` 回到主线程，避免行为分裂
- 当前阶段只做**单 DBApp**，不做 `DBAppMgr` 分片和多 DBApp 一致性

---

## 验收状态（2026-04-12）

- [x] `IDatabase` 接口已落地，包含 `put/get/del/lookup/checkout/clear_checkout/auto_load`
- [x] `DBApp` 进程已落地，可启动、注册到 `machined`、处理数据库消息
- [x] `XmlDatabase` 已落地，并具备延迟回调与缓冲 flush 能力
- [x] `CheckoutManager` 已落地，支持 `Checking / Confirmed` 两阶段内存占位
- [x] `DBApp` 已支持 `AbortCheckout`，用于登录回滚场景
- [x] `DBApp` 已能处理 `LoginApp -> DBApp` 的 `AuthLogin`
- [x] `EntityDefRegistry` 已支持 `DBApp` 从 `entity_defs.json` 加载
- [x] 单元测试已覆盖 `dbapp_messages` 与 `xml_database`
- [~] `SQLite` backend 方案已形成草案，但尚未实现
- [ ] `MySQL` backend 尚未作为当前主线完成落地
- [ ] 仍缺少完整的 `DBApp` 端到端集成测试
- [ ] 仍未进入多 `DBApp` / `DBAppMgr` 分片阶段

---

## 当前代码现实

### 1. 已存在模块

当前仓库中与 Phase 7 直接相关的实现包括:

- `src/lib/db/idatabase.hpp`
- `src/lib/db/database_factory.cpp`
- `src/lib/db_xml/xml_database.hpp`
- `src/lib/db_xml/xml_database.cpp`
- `src/server/dbapp/dbapp.hpp`
- `src/server/dbapp/dbapp.cpp`
- `src/server/dbapp/dbapp_messages.hpp`
- `src/server/dbapp/checkout_manager.hpp`
- `src/server/dbapp/checkout_manager.cpp`

### 2. 当前 DBApp 职责

当前 `DBApp` 已承担以下职责:

- 从 `entity_defs.json` 加载 `EntityDefRegistry`
- 按 `ServerConfig.db_type` 创建数据库后端
- 接收并处理:
  - `WriteEntity`
  - `CheckoutEntity`
  - `CheckinEntity`
  - `DeleteEntity`
  - `LookupEntity`
  - `AbortCheckout`
  - `AuthLogin`
- 监听 `BaseApp` 死亡并清理内存 / 数据库中的 checkout
- 在 `on_tick_complete()` 中统一 pump `database_->process_results()`

### 3. 当前后端能力

当前实际可用的数据库后端是:

- `xml`

当前代码中对 `mysql` 有工厂入口预留，但文档层不能再把它写成“已完成能力”。

---

## 后端策略

### 1. 当前推荐顺序

Atlas 当前数据库后端策略调整为:

1. `sqlite`
   开发默认后端候选，目标是替代 XML 承担日常开发和本地验证
2. `xml`
   最轻量 fallback，用于最小 smoke test 和无依赖启动
3. `mysql`
   正式后端预选，用于后续更接近生产的部署

### 2. 为什么开发阶段优先 SQLite

和 `xml` 相比，`sqlite` 更适合作为开发阶段主后端，因为它:

- 更接近关系型数据库和事务语义
- 更自然支持 `lookup_by_name` 与唯一约束
- 更自然支持 `checkout_entity` 这类“查询 + 占用”原子操作
- 不需要外部服务，仍然保持本地开发零依赖
- 能更早暴露和 `mysql` 接近的数据层问题

### 3. XML 的定位

`xml` 不删除，但降级为:

- fallback backend
- 最小化启动路径
- 文件级 smoke test backend

它不再适合作为默认开发后端的长期主线。

### 4. 为什么当前不选 MongoDB

`MongoDB` 不是当前主线方案，原因很具体:

- 当前 `IDatabase` 的核心语义是 `identifier lookup + checkout ownership + auto_load + blob`
- 这些语义更接近关系型表结构，而不是文档库模型
- `checkout` 的原子占用、唯一键和后续与 `MySQL` 的迁移一致性，关系型更自然
- 开发阶段引入 MongoDB 会增加额外外部服务依赖，但收益不如 SQLite 明确

所以当前文档定论是:

- **MongoDB 可以做实验分支**
- **不作为 Phase 7 主线交付**

---

## 数据模型结论

### 1. 统一抽象

无论底层是 `xml`、`sqlite` 还是 `mysql`，Phase 7 的逻辑抽象保持一致:

- 一个实体有:
  - `dbid`
  - `type_id`
  - `identifier`
  - `blob`
  - `password_hash`
  - `auto_load`
  - `checkout owner`

### 2. 关系型后端推荐模型

对 `sqlite` / `mysql`，统一采用:

- **`blob + 少量索引列 + checkout 状态列`**

而不是“每个持久属性一列”的 BigWorld 式复杂映射。

推荐核心字段:

- `dbid`
- `type_id`
- `identifier`
- `password_hash`
- `blob`
- `auto_load`
- `checked_out`
- `checkout_addr`
- `checkout_app_id`
- `checkout_eid`

### 3. XML 后端映射

`xml` 后端继续使用文件系统映射这些语义:

- `blob` 持久化到实体文件
- `identifier` 维护索引
- `checkout` / `auto_load` 通过元信息维护

但文档上应明确承认:

- XML 只是语义兼容层
- 不是后续数据库主路径的目标形态

---

## 当前接口与协议边界

### 1. `IDatabase`

当前 `IDatabase` 已形成稳定主接口，后续新增后端必须兼容这些语义:

- `put_entity`
- `get_entity`
- `del_entity`
- `lookup_by_name`
- `checkout_entity`
- `checkout_entity_by_name`
- `clear_checkout`
- `clear_checkouts_for_address`
- `mark_checkout_cleared`
- `get_auto_load_entities`
- `set_auto_load`
- `set_deferred_mode`
- `process_results`

关键约束:

- 回调必须在主线程可控位置交付
- `checkout_entity` 必须表达“成功 / 未找到 / 已被占用”
- `clear_checkouts_for_address` 必须支持 BaseApp 死亡清理

### 2. 当前 DBApp 消息面

当前 Phase 7 已落地的 DBApp 消息是:

- `WriteEntity` / `WriteEntityAck`
- `CheckoutEntity` / `CheckoutEntityAck`
- `CheckinEntity`
- `DeleteEntity` / `DeleteEntityAck`
- `LookupEntity` / `LookupEntityAck`
- `AbortCheckout` / `AbortCheckoutAck`

这些消息已经被 Phase 9 的登录链路依赖，不能再轻率重写。

### 3. 登录相关边界

当前 `DBApp` 还承担:

- `LoginApp -> DBApp` 的 `AuthLogin`

但需要明确边界:

- `DBApp` 只负责账号查找 / 认证数据读取 / 账号自创建
- `DBApp` 不负责客户端连接
- `DBApp` 不负责 `BaseApp` 分配
- `DBApp` 不负责重复登录协商

---

## 工业级实施原则

### 1. 统一异步契约

即使后端内部实现是同步的，`DBApp` 对上层也必须保持统一行为:

- 结果通过 `process_results()` 回到主线程
- 不允许一部分后端“同步立即回调”，另一部分后端“异步排队回调”

否则登录回滚和状态机在不同后端下会出现行为差异。

### 2. Checkout 必须有 owner 语义

`checkout` 不是一个简单布尔标志，而是实体所有权。

至少必须能表达:

- 谁持有这个实体
- 是否处于 `Checking`
- 是否已经 `Confirmed`
- BaseApp 死亡时如何回收

### 3. Fail-safe 清理

必须明确处理以下路径:

- `AbortCheckout`
- `WriteFlags::LogOff`
- `CheckinEntity`
- `BaseApp` 死亡通知
- `DBApp` 自身关闭时的有序清理

### 4. 后端切换不能改上层协议

从 `xml` 切到 `sqlite`，再从 `sqlite` 切到 `mysql` 时:

- `DBApp` 消息协议不应重写
- `BaseApp` / `LoginApp` 状态机不应感知后端差异

---

## 本阶段最终范围

### 进入当前 Phase 7 主线的内容

- 单 `DBApp` 进程继续作为唯一数据库代理
- 维持并加固现有 `IDatabase` 接口
- 保持 `xml` 可用
- 新增 `sqlite` backend 并提升为开发默认候选
- 保留 `mysql` 作为正式后端预选接口和文档路径
- 补齐针对 `DBApp` 当前真实消息面的测试和实施文档

### 不进入当前范围的内容

- 多 `DBApp`
- `DBAppMgr`
- 分片 / Rendezvous Hash
- `MongoDB` 主线支持
- 大规模 schema migration 框架
- 外部认证系统替代

---

## 实施计划

按当前代码现实，Phase 7 后续应分四段推进。

### P7.1 基线收敛

目标:

- 把文档、配置、默认策略与当前仓库真实状态对齐

交付:

- `phase07_dbapp.md` 改为当前实施版
- DB 后端策略统一写成 `sqlite / xml / mysql`
- 不再在主文档中把 `mysql` 写成已落地能力

状态:

- 本文档更新后视为完成

### P7.2 SQLite backend 落地

目标:

- 实现开发阶段默认数据库后端候选

范围:

- 扩展 `DatabaseConfig`
- 扩展 `ServerConfig`
- 更新 `database_factory.cpp`
- 新增 `src/lib/db_sqlite/`
- 实现:
  - `put_entity`
  - `get_entity`
  - `del_entity`
  - `lookup_by_name`
  - `checkout_entity`
  - `checkout_entity_by_name`
  - `clear_checkout`
  - `clear_checkouts_for_address`
  - `get_auto_load_entities`
  - `set_auto_load`
  - `process_results`

完成标准:

- `DBApp` 可用 `sqlite` 启动
- 登录主链路可在 `sqlite` 后端下跑通
- checkout 占用与释放语义稳定

### P7.3 DBApp 集成补强

目标:

- 让 `DBApp` 在登录回滚和 BaseApp 故障路径下更稳

范围:

- 校验 `AbortCheckout` 与 delayed callback 的组合行为
- 校验 `mark_checkout_cleared` fast path 在新后端下的语义
- 增加 `DBApp` watcher / metrics 的使用说明
- 补一组端到端集成测试

完成标准:

- `CheckoutEntity -> AbortCheckout`
- `CheckoutEntity -> CheckinEntity`
- `BaseApp death -> clear_checkouts_for_address`
- `AuthLogin` 基线路径

都具备稳定回归测试

### P7.4 MySQL backend 进入正式实现

目标:

- 让 `mysql` 成为正式部署预选

范围:

- 新增 `src/lib/db_mysql/`
- 建表、索引、唯一约束、事务实现
- 对齐 `sqlite` 的数据模型和语义

完成标准:

- 消息面与 `sqlite/xml` 完全一致
- 行为差异只体现在性能和运维，不体现在协议语义

---

## 配置策略

### 1. 当前推荐配置

开发阶段推荐:

```json
{
  "db_type": "sqlite",
  "db_sqlite_path": "data/atlas_dev.sqlite3"
}
```

最小 fallback:

```json
{
  "db_type": "xml",
  "db_xml_dir": "data/db"
}
```

正式后端预选:

```json
{
  "db_type": "mysql",
  "db_mysql_host": "127.0.0.1",
  "db_mysql_port": 3306,
  "db_mysql_user": "atlas",
  "db_mysql_password": "secret",
  "db_mysql_database": "atlas"
}
```

### 2. `ServerConfig` 方向

为支持 Phase 7 主线，`ServerConfig` 需要演进为同时支持:

- `db_xml_dir`
- `db_sqlite_path`
- `db_sqlite_wal`
- `db_sqlite_busy_timeout_ms`
- `db_mysql_*`

其中:

- `sqlite` 是开发优先项
- `mysql` 是正式部署优先项

---

## 测试要求

### 1. 已存在测试

当前已存在:

- `tests/unit/test_xml_database.cpp`
- `tests/unit/test_dbapp_messages.cpp`

这说明消息层和 XML backend 已经有基础覆盖，但还不够。

### 2. 本阶段必须新增

建议新增:

- `tests/unit/test_checkout_manager.cpp`
- `tests/unit/test_sqlite_database.cpp`
- `tests/integration/test_dbapp_login_flow.cpp`
- `tests/integration/test_dbapp_checkout_cleanup.cpp`

### 3. 必测场景

至少覆盖:

- create / update / get / delete
- `lookup_by_name`
- `password_hash` 的持久化与读回
- `checkout_entity`
- 重复 checkout 返回 owner
- `AbortCheckout`
- `CheckinEntity`
- `WriteFlags::LogOff`
- `BaseApp` 死亡后的批量 checkout 清理
- `auto_load`
- `sqlite` 后端下的登录基线路径

---

## Definition Of Done

Phase 7 只有满足以下条件，才应视为真正完成:

- 单 `DBApp` 架构稳定可运行
- `IDatabase` 语义不再频繁变动
- `xml` 继续可用但降级为 fallback
- `sqlite` 成为可实际使用的开发默认后端候选
- `mysql` 的接口与数据模型路径明确，不再与开发路径冲突
- `DBApp` 的 checkout / abort / clear 路径有回归测试
- Phase 9 登录主链路可在 `sqlite` 后端上稳定验证

---

## 与其他文档的边界

### 本文档负责

- `DBApp` 的角色与边界
- 数据库后端策略
- `IDatabase` 语义
- 单 DBApp 实施路线
- `sqlite/xml/mysql` 的阶段定位

### 本文档不负责

- 多 `LoginApp` / `LoginAppMgr` 设计
- 登录入口负载均衡
- `BaseApp` 侧重复登录状态机
- `CellApp` / space 持久化策略

相关文档:

- [phase09_login_flow.md](./phase09_login_flow.md)
- [../MULTI_LOGINAPP_DESIGN.md](../MULTI_LOGINAPP_DESIGN.md)

---

## 当前结论

截至 2026-04-12，Phase 7 不应再被描述为“从零设计 DBApp”。

更准确的说法是:

- **DBApp 主体已经落地**
- **XML backend 已经可用**
- **下一步主线是把开发默认后端切到 SQLite**
- **MySQL 继续作为正式后端预选推进**

这才是当前 Atlas 数据库层最可落地、也最符合工程现实的方案。
