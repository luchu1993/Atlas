# SQLite Backend 草案

> 日期: 2026-04-12
> 状态: 草案
> 适用范围: Atlas 开发阶段数据库后端替代 XML backend

---

## 1. 背景

当前 Atlas 的数据库后端抽象为 `IDatabase`，现有后端包括:

- `xml` 后端: 主要用于开发 / 测试，无外部数据库依赖
- `mysql` 后端: 作为正式后端预选

随着登录链路和 checkout / checkin 语义逐步完整，XML backend 的局限越来越明显:

- 文件 I/O 和索引文件维护成本高
- checkout / checkin 原子语义需要手工维持
- 写回、索引、checkout 状态不在同一事务边界内
- 高压登录场景下 staged / coalesced writeback 逻辑已经显著复杂化

因此，开发阶段需要一个比 XML backend 更稳、更接近 MySQL 语义、但仍然保持“零外部服务依赖”的方案。

本草案的结论是:

- **新增 `sqlite` backend，作为开发阶段默认后端候选**
- 保留 `xml` 作为最轻量 fallback
- 保留 `mysql` 作为正式后端预选

---

## 2. 目标

SQLite backend 的目标是:

- 保持开发机零外部服务依赖
- 替代 XML backend 承担日常开发 / 本地验证
- 更稳定地支持:
  - `lookup_by_name`
  - `checkout_entity`
  - `clear_checkout`
  - `auto_load`
  - `blob` 持久化
- 尽量贴近 MySQL 的事务语义，降低未来切换成本

---

## 3. 非目标

首版不做以下事情:

- 多 `DBApp` 共享同一个 SQLite 文件
- 分布式高可用
- 复杂 migration 框架
- 文档化拆表建模
- 局部字段级更新优化
- 跨进程写并发优化

首版只追求:

- **开发阶段比 XML 更稳**
- **接口语义和 MySQL 更接近**

---

## 4. 当前接口约束

SQLite backend 必须适配现有 `IDatabase` 接口:

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

相关接口定义见:

- [src/lib/db/idatabase.hpp](/D:/GameEngine/Atlas/src/lib/db/idatabase.hpp)

当前数据模型的核心特征是:

- 主体实体数据存为 `blob`
- 额外索引字段很少，主要是 `identifier`
- 登录认证还需要 `password_hash`
- checkout 所有权通过 `CheckoutInfo` 表示

这意味着 SQLite backend 不需要做文档数据库式建模，而应沿用“**blob + 少量索引列 + checkout 状态列**”的方案。

---

## 5. 配置草案

### 5.1 `DatabaseConfig`

建议在 `DatabaseConfig` 中新增:

```cpp
std::filesystem::path sqlite_path{"data/atlas_dev.sqlite3"};
bool sqlite_wal{true};
int sqlite_busy_timeout_ms{5000};
bool sqlite_foreign_keys{true};
```

### 5.2 `ServerConfig`

建议同步在 `ServerConfig` 中新增:

```cpp
std::filesystem::path db_sqlite_path{"data/atlas_dev.sqlite3"};
bool db_sqlite_wal{true};
int db_sqlite_busy_timeout_ms{5000};
```

### 5.3 JSON 配置示例

```json
{
  "db": {
    "type": "sqlite",
    "sqlite_path": "data/atlas_dev.sqlite3",
    "sqlite_wal": true,
    "sqlite_busy_timeout_ms": 5000
  }
}
```

---

## 6. 目录结构

建议新增:

```text
src/lib/db_sqlite/
├── CMakeLists.txt
├── sqlite_database.hpp
├── sqlite_database.cpp
└── sqlite_statement.hpp      // 可选，轻量 RAII 包装
```

同时修改:

- `src/lib/db/database_factory.cpp`
- `src/lib/server/server_config.hpp`
- `src/lib/server/server_config.cpp`
- `src/lib/db/CMakeLists.txt`

---

## 7. 数据模型

### 7.1 单文件数据库

开发阶段使用单 SQLite 文件:

```text
data/atlas_dev.sqlite3
```

### 7.2 主表 `entities`

建议主表如下:

```sql
CREATE TABLE entities (
    dbid            INTEGER PRIMARY KEY,
    type_id         INTEGER NOT NULL,
    blob            BLOB NOT NULL,
    identifier      TEXT,
    password_hash   TEXT,
    auto_load       INTEGER NOT NULL DEFAULT 0,
    checked_out     INTEGER NOT NULL DEFAULT 0,
    checkout_addr   TEXT,
    checkout_app_id INTEGER NOT NULL DEFAULT 0,
    checkout_eid    INTEGER NOT NULL DEFAULT 0,
    created_at_ms   INTEGER NOT NULL,
    updated_at_ms   INTEGER NOT NULL
);
```

字段解释:

- `dbid`: 全局数据库 ID
- `type_id`: 实体类型
- `blob`: 持久化实体二进制
- `identifier`: `[Identifier]` 字段值
- `password_hash`: 登录认证所需密码哈希
- `auto_load`: 是否开服自动加载
- `checked_out`: 是否已被某个 `BaseApp` checkout
- `checkout_addr` / `checkout_app_id` / `checkout_eid`: 当前 owner

### 7.3 元信息表 `meta`

```sql
CREATE TABLE meta (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
);
```

用于保存:

- `schema_version`
- 可选的其他 backend 元信息

---

## 8. 索引设计

建议索引如下:

```sql
CREATE INDEX idx_entities_type_dbid
ON entities(type_id, dbid);

CREATE INDEX idx_entities_type_identifier
ON entities(type_id, identifier);

CREATE UNIQUE INDEX idx_entities_type_identifier_unique
ON entities(type_id, identifier)
WHERE identifier IS NOT NULL AND identifier <> '';

CREATE INDEX idx_entities_auto_load
ON entities(auto_load, type_id, dbid);

CREATE INDEX idx_entities_checkout_addr
ON entities(checked_out, checkout_addr);
```

说明:

- `type_id + identifier` 必须支持按名查找
- `identifier` 唯一约束可防止同类型下重名
- `checkout_addr` 索引用于 `clear_checkouts_for_address`

---

## 9. 接口映射

### 9.1 `put_entity`

语义要求:

- `CreateNew`: 新建实体并分配 `dbid`
- 普通写入: 更新 blob/identifier/password_hash
- `LogOff`: 更新实体并顺带清除 checkout
- `Delete`: 删除实体
- `AutoLoadOn/Off`: 更新 `auto_load`

建议实现:

- 所有写操作放入单事务
- `CreateNew` 使用 SQLite `INTEGER PRIMARY KEY` 自增能力
- `updated_at_ms` 每次写更新

### 9.2 `get_entity`

```sql
SELECT
    dbid, type_id, blob, identifier, password_hash,
    checked_out, checkout_addr, checkout_app_id, checkout_eid
FROM entities
WHERE dbid = ? AND type_id = ?
LIMIT 1;
```

返回:

- `blob`
- `identifier`
- `password_hash`
- 若 `checked_out = 1`，则构造 `checked_out_by`

### 9.3 `lookup_by_name`

```sql
SELECT dbid, password_hash
FROM entities
WHERE type_id = ? AND identifier = ?
LIMIT 1;
```

### 9.4 `checkout_entity`

这是 SQLite backend 的关键能力。

目标语义:

- 原子“检查是否已 checkout + 占用 checkout”

建议步骤:

1. `BEGIN IMMEDIATE`
2. 查询目标实体
3. 若不存在，返回失败
4. 若 `checked_out = 1`，返回 `checked_out_by`
5. 否则执行:

```sql
UPDATE entities
SET checked_out = 1,
    checkout_addr = ?,
    checkout_app_id = ?,
    checkout_eid = ?,
    updated_at_ms = ?
WHERE dbid = ? AND type_id = ? AND checked_out = 0;
```

6. 检查受影响行数
7. `COMMIT`

### 9.5 `checkout_entity_by_name`

和 `checkout_entity` 相同，只是通过 `type_id + identifier` 定位目标行。

### 9.6 `clear_checkout`

```sql
UPDATE entities
SET checked_out = 0,
    checkout_addr = NULL,
    checkout_app_id = 0,
    checkout_eid = 0,
    updated_at_ms = ?
WHERE dbid = ? AND type_id = ?;
```

### 9.7 `clear_checkouts_for_address`

```sql
UPDATE entities
SET checked_out = 0,
    checkout_addr = NULL,
    checkout_app_id = 0,
    checkout_eid = 0,
    updated_at_ms = ?
WHERE checked_out = 1 AND checkout_addr = ?;
```

### 9.8 `get_auto_load_entities`

```sql
SELECT dbid, type_id, blob, identifier
FROM entities
WHERE auto_load = 1
ORDER BY type_id, dbid;
```

### 9.9 `set_auto_load`

```sql
UPDATE entities
SET auto_load = ?, updated_at_ms = ?
WHERE dbid = ? AND type_id = ?;
```

---

## 10. 线程模型与回调模型

### 10.1 首版建议

首版建议采用最简单实现:

- 单 SQLite 连接
- `DBApp` 驱动
- SQL 可同步执行
- callback 不直接在调用点回调，而是像异步后端一样排队到 `process_results()`

这样做的好处:

- 调用语义与 MySQL backend 更接近
- 避免 XML backend 那种“同步后端 / 异步后端”行为差异过大
- 更利于测试稳定性

### 10.2 后续可选扩展

如有必要，后续可扩展为:

- 单 worker 线程
- 串行 SQL 队列
- 主线程 `process_results()` 投递回调

但首版不建议直接做复杂异步框架。

---

## 11. SQLite PRAGMA 建议

启动时建议设置:

```sql
PRAGMA journal_mode = WAL;
PRAGMA synchronous = NORMAL;
PRAGMA foreign_keys = ON;
PRAGMA busy_timeout = 5000;
```

建议解释:

- `WAL`: 提升开发阶段写入和恢复体验
- `NORMAL`: 比 `FULL` 更快，开发阶段足够
- `busy_timeout`: 减少短时锁竞争导致的立即失败

---

## 12. 与 XML backend 的差异

相对 XML backend，SQLite backend 的直接收益是:

- checkout / checkin 原子语义更自然
- identifier 索引不需要单独维护 `index.json`
- 元数据不需要单独维护多个文件
- 写回和状态更新可以进入同一个事务
- 更接近 MySQL 的 schema/索引/唯一约束思路
- 更适合当前登录链路这种“状态机 + 所有权”场景

换句话说，SQLite 不是为了“更大规模”，而是为了让开发后端更像真正的数据库。

---

## 13. 与 MySQL backend 的关系

SQLite backend 的定位不是替代 MySQL，而是:

- 开发阶段默认后端候选
- 本地验证和单机调试用
- 帮助尽早验证关系型/事务型语义

MySQL backend 仍然是:

- 正式后端预选
- 更接近线上部署模式
- 后续性能、并发和运维主路径

推荐后端分层:

1. `sqlite`: 开发默认
2. `xml`: 最轻量 fallback / 最简 smoke test
3. `mysql`: 正式后端预选

---

## 14. 实现步骤

### P1 配置与工厂

- 扩展 `DatabaseConfig`
- 扩展 `ServerConfig`
- 修改 `database_factory.cpp`
- 支持 `db_type = "sqlite"`

### P2 SQLite backend 启动

- 新增 `sqlite_database.hpp/.cpp`
- `startup()` 中建库、建表、建索引
- 写入/读取 `schema_version`

### P3 CRUD

- `put_entity`
- `get_entity`
- `del_entity`
- `lookup_by_name`

### P4 checkout 相关

- `checkout_entity`
- `checkout_entity_by_name`
- `clear_checkout`
- `clear_checkouts_for_address`

### P5 auto-load 与主线程回调

- `get_auto_load_entities`
- `set_auto_load`
- `process_results`

### P6 测试与接入验证

- 单元测试
- DBApp 启动验证
- 登录主链路验证

---

## 15. 测试建议

建议新增:

```text
tests/unit/test_sqlite_database.cpp
```

至少覆盖:

- create / update / get / delete
- identifier lookup
- password_hash 读回
- checkout 成功
- 重复 checkout 返回 owner
- clear_checkout
- clear_checkouts_for_address
- auto_load
- `CreateNew`
- `LogOff`
- `Delete`
- `AutoLoadOn/Off`

补充建议:

- 用 SQLite backend 跑一轮当前登录主链路 smoke test
- 用 SQLite backend 跑一轮登录重登回滚相关定向测试

---

## 16. 风险与注意事项

### 16.1 单文件锁竞争

SQLite 虽然适合开发，但仍然是单文件数据库。

这意味着:

- 不适合作为多 `DBApp` 正式后端
- 不适合作为高并发线上后端

### 16.2 行为差异

尽管 SQLite 比 XML 更接近 MySQL，但仍然有差异:

- 类型系统不同
- 锁模型不同
- SQL 方言细节不同

所以 SQLite 的目标是“开发更稳、更像正式库”，不是“完全模拟 MySQL”。

### 16.3 `password_hash` 来源

当前接口已有 `LookupResult.password_hash` 字段，因此 SQLite backend 需要和 MySQL 一样，
明确保存并返回该字段，而不是只依赖 blob。

---

## 17. 当前建议

基于 Atlas 当前现状，推荐结论是:

- **短期:** 保持 XML backend 可用，但不再作为首选开发后端
- **中期:** 引入 SQLite backend，作为开发默认
- **长期:** 继续推进 MySQL backend 作为正式后端

如果要在开发阶段从 XML 往前走一步，SQLite 是当前最合适、最稳妥的方向。
