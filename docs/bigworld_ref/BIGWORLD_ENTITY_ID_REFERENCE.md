# BigWorld EntityID 一致性机制参考 — Atlas 实现指南

> 来源: BigWorld Engine 14.4.1 源码分析
> 关联: [BigWorld RPC 参考](BIGWORLD_RPC_REFERENCE.md) | [BigWorld EventHistory 参考](BIGWORLD_EVENT_HISTORY_REFERENCE.md) | [Phase 7 DBApp](../roadmap/phase07_dbapp.md) | [Phase 8 BaseApp](../roadmap/phase08_baseapp.md)

---

## 1. 概述

BigWorld 中 EntityID 在 Client、BaseApp、CellApp **三端完全一致**。源码注释明确说明：

> "An entity's ID is constant between all its instances on the baseapp, cellapp and client. EntityIDs are however not persistent and should be assumed to be different upon each run of the server."
>
> — `lib/network/basictypes.hpp:91-100`

即：同一个实体在所有组件上使用同一个 `int32` 值标识，但 ID 不跨服务器重启持久化。

---

## 2. ID 类型与值域

> 源文件: `lib/network/basictypes.hpp:104-124`

```cpp
typedef int32 EntityID;                              // 有符号 32 位整数

const EntityID NULL_ENTITY_ID        = 0;            // 无效 ID
const EntityID FIRST_ENTITY_ID       = 1;            // 最小有效 ID
const EntityID FIRST_LOCAL_ENTITY_ID = 0x7F000000;   // 客户端本地实体起始
```

ID 空间划分：

```
0x00000001 ────────────────────── 0x7EFFFFFF     0x7F000000 ────── 0x7FFFFFFF
│          服务端分配的 ID                    │   │     客户端本地 ID            │
│  Client / Base / Cell 三端一致             │   │     仅客户端使用              │
│  由 DBApp 统一管理                         │   │     客户端自增分配             │
└────────────────────────────────────────────┘   └────────────────────────────┘
```

---

## 3. 分层分配架构

EntityID 的分配采用**分层代理模式（Hierarchical Broker Pattern）**：

```
┌──────────────────────────────────────────────────────────┐
│                        DBApp                              │
│              (EntityID 的唯一权威来源)                      │
│                                                          │
│   数据库中维护 ID 池                                       │
│   接口: getIDs(numIDs) → 返回一批 ID                      │
│         putIDs(ids[])  → 回收不用的 ID                    │
└─────────┬────────────────────────┬───────────────────────┘
          │ getIDs (批量请求)       │ getIDs (批量请求)
          ▼                        ▼
   ┌─────────────┐          ┌─────────────┐
   │   BaseApp    │          │   CellApp   │
   │  IDClient    │          │  IDClient   │
   │ (本地缓存池) │          │ (本地缓存池) │
   └──────┬──────┘          └─────────────┘
          │
          │ getID() → 从缓存取一个
          ▼
    分配给新创建的实体
```

### 3.1 IDClient — 本地 ID 代理

> 源文件: `lib/server/id_client.hpp:15-109`

```cpp
class IDClient
{
public:
    EntityID getID();            // 从本地队列取一个 ID
    void putUsedID( EntityID );  // 归还不用的 ID
    void returnIDs();            // 归还所有 ID

private:
    // 水位控制参数
    int criticallyLowSize_;      // 紧急阈值：低于此值立即向 DBApp 请求
    int lowSize_;                // 低水位：低于此值开始预取
    int desiredSize_;            // 目标缓存数量
    int highSize_;               // 高水位：超过此值归还多余的
};
```

水位机制示意：

```
ID 缓存数量:
  │
  │  highSize_ ─────  超过此值 → 归还多余 ID 给 DBApp
  │                │
  │  desiredSize_  │  目标值：预取到这个数量
  │                │
  │  lowSize_ ─────  低于此值 → 开始异步预取
  │                │
  │  criticallyLow │  低于此值 → 阻塞等待 DBApp 响应
  │                │
  0 ───────────────
```

### 3.2 DBApp 端的 ID 生成

> 源文件: `server/dbapp/dbapp.cpp:2636-2648`、`lib/db/dbapp_interface.hpp:105-109`

DBApp 从数据库查询可用 ID 并批量返回：

- MySQL 后端: 从 `bigworldEntityID` 表获取可用 ID 范围
- XML 后端: 扫描已有实体，找到未使用的 ID

---

## 4. ID 在三端的传播流程

### 4.1 完整生命周期

```
步骤1: BaseApp 创建实体
━━━━━━━━━━━━━━━━━━━━━━━━
  EntityCreator::createBase()
    │
    ├─ id = idClient_.getID()     ← 从缓存池取 ID，假设 id=1042
    └─ 创建 Base 实体 (id=1042)


步骤2: Base → Cell 传播
━━━━━━━━━━━━━━━━━━━━━━━━
  BaseApp 发送消息给 CellApp:
  ┌─────────────────────────────┐
  │ CellAppInterface::          │
  │   createEntity              │
  │                             │
  │   EntityID id = 1042  ←──── │── 同一个 ID 直接传递
  │   position, properties...   │
  └─────────────────────────────┘
  CellApp 用 id=1042 创建 Cell 实体


步骤3: Cell → Client 传播
━━━━━━━━━━━━━━━━━━━━━━━━━
  CellApp (通过 Witness) 发送给客户端:
  ┌─────────────────────────────┐
  │ ClientInterface::           │
  │   createEntity              │
  │                             │
  │   EntityID id = 1042  ←──── │── 同一个 ID 再传给客户端
  │   type, properties...       │
  └─────────────────────────────┘
  Client 用 id=1042 创建本地实体对象
```

### 4.2 从数据库加载已有实体

> 源文件: `server/baseapp/entity_creator.cpp:1031-1043`

```cpp
EntityID id = ...; // 从数据库消息中读取
if (id == 0)
{
    id = idClient_.getID();   // 新实体，分配新 ID
}
// 已有实体（如登录恢复）直接使用数据库中的 ID
```

---

## 5. 客户端 IDAlias 带宽优化

### 5.1 问题

EntityID 是 4 字节（int32）。在频繁的位置/属性更新消息中，每个玩家 AOI 内可能有几十到上百个实体，每帧每个实体的更新都带 4 字节 ID 开销很大。

### 5.2 解决方案：1 字节别名

> 源文件: `lib/network/msgtypes.hpp:397`、`lib/connection/server_connection.hpp:506`

```cpp
typedef uint8 IDAlias;           // 0~255，最多同时引用 256 个实体

// 客户端维护查找表
EntityID idAlias_[ 256 ];        // 别名 → 真实 EntityID
```

### 5.3 工作流程

```
服务端                                          客户端
  │                                               │
  │  enterAoI(EntityID=1042, IDAlias=7)           │
  │ ─────────────────────────────────────────────► │
  │    "实体1042进入视野，以后用别名7代替"            │
  │                                  idAlias_[7] = 1042
  │                                               │
  │  avatarUpdateAliasDetailed(IDAlias=7, pos, dir)│
  │ ─────────────────────────────────────────────► │
  │    只用 1 字节！节省 3 字节/条消息                │
  │                                               │
  │                         查表: idAlias_[7] → 1042
  │                         更新实体 1042 的位置     │
```

关键代码：

```cpp
// server_connection.cpp:2552
void ServerConnection::enterAoI( EntityID id, IDAlias idAlias )
{
    idAlias_[ idAlias ] = id;
    // ...
}
```

**IDAlias 只是网络传输优化，不影响 EntityID 的全局一致性。客户端内部逻辑仍使用真实 EntityID。**

### 5.4 带宽节省分析

以 AOI 内 50 个实体、30Hz 更新为例：

```
无 IDAlias:  50 × 4 bytes × 30 = 6,000 bytes/sec  (仅 ID 部分)
有 IDAlias:  50 × 1 byte  × 30 = 1,500 bytes/sec
节省:        4,500 bytes/sec per client (75%)
```

---

## 6. 客户端本地实体

> 源文件: `client/bw_entities.cpp:38, 498`

客户端可创建不与服务器同步的纯本地实体（如特效、UI 元素）：

```cpp
static EntityID s_nextLocalEntityID_ = FIRST_LOCAL_ENTITY_ID;  // 0x7F000000

EntityID newID = s_nextLocalEntityID_++;   // 客户端自行递增
```

特点：
- ID ≥ `0x7F000000`，与服务端分配的 ID 空间不冲突
- 仅存在于客户端，服务端完全不感知
- 检测方式: `if (entityID >= FIRST_LOCAL_ENTITY_ID)` 即为本地实体

---

## 7. ID 回收与锁定

> 源文件: `lib/server/id_client.cpp`

```cpp
// #define MF_ID_RECYCLING  ← 默认关闭

// 如果开启，有 10 秒锁定期防止立即复用:
unlockTime_ = timestamp() + uint64(10) * stampsPerSecond();
```

- **默认不回收 ID**，避免竞态：客户端可能还在引用已销毁实体的 ID
- 可选开启回收，但有 10 秒冷却期
- 使用 `int32` 而非 `uint16` 就是为了有足够空间避免回绕

---

## 8. 总结

| 问题 | 答案 |
|---|---|
| 三端 EntityID 一致？ | **完全一致**，同一个 `int32` 值 |
| 谁分配 ID？ | **DBApp** 是唯一权威源 |
| 怎么传播？ | BaseApp 分配 → 消息传 CellApp → 消息传 Client，原值传递 |
| IDAlias 是什么？ | 1 字节网络别名，不影响 ID 一致性 |
| 本地实体？ | `≥ 0x7F000000` 的独立空间，与服务端不冲突 |
| 会复用？ | 默认不复用，可选 10 秒冷却回收 |
| 跨重启持久？ | 不持久，每次启动重新分配 |

---

## 9. Atlas 实现建议

### 9.1 核心设计要点

| 设计点 | BigWorld 方案 | Atlas 建议 |
|---|---|---|
| ID 类型 | `int32` | 建议 `uint64`，彻底避免回绕和空间不足 |
| 分配源 | DBApp 中心化 | 保留此设计，DBApp 统一分配保证全局唯一 |
| 本地缓存 | IDClient 水位控制 | 保留此设计，批量预取减少跨进程请求 |
| 三端一致 | 原值传递 | **必须保持**，这是实体系统正确性的基础 |
| 客户端别名 | `uint8` IDAlias (256个) | 可扩展到 `uint16` (65536) 以支持更大 AOI |
| 本地实体 | 高位 ID 空间 | 保留此设计，高位区间留给客户端本地实体 |
| ID 回收 | 默认关闭 | 使用 `uint64` 后基本无需回收 |

### 9.2 关键不变量

实现时必须保证以下不变量：

1. **全局唯一性** — 任何时刻不能有两个不同实体使用同一 EntityID
2. **三端一致性** — Base、Cell、Client 上的同一实体必须使用相同 ID，任何转换/映射都是 bug
3. **ID 空间隔离** — 服务端 ID 和客户端本地 ID 必须使用不重叠的区间
4. **分配原子性** — `getID()` 在并发环境下必须保证不返回重复 ID
5. **ID 不跨越边界传播时丢失** — 实体迁移（Cell → Cell、Base → Base）时 ID 必须保持不变

### 9.3 与 BigWorld 的差异建议

1. **使用 `uint64`**: BigWorld 用 `int32` 是历史原因（2000 年代设计）。Atlas 用 `uint64` 可以：
   - 不再需要 ID 回收机制
   - 高 32 位可编码服务器 ID，支持多集群
   - 低 32 位自增，单集群支持 40 亿+ 实体

2. **IDAlias 可升级为 `uint16`**: BigWorld 的 256 上限在大规模场景下可能不够。`uint16` 支持 65536 个同时可见实体，每条消息仅多 1 字节。

3. **无锁 ID 分配**: BigWorld 的 IDClient 是单线程的。Atlas 如果使用多线程 BaseApp，可考虑 `std::atomic<uint64>` 分配或分段预取。
