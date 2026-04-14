# BigWorld EventHistory 属性同步机制参考 — Atlas 实现指南

> 来源: BigWorld Engine 14.4.1 源码分析
> 关联: [BigWorld RPC 参考](BIGWORLD_RPC_REFERENCE.md) | [BigWorld EntityID 参考](BIGWORLD_ENTITY_ID_REFERENCE.md) | [Phase 10 CellApp](roadmap/phase10_cellapp.md) | [Phase 12 Client SDK](roadmap/phase12_client_sdk.md)

---

## 1. 概述

### 1.1 什么是 EventHistory

`EventHistory` 是 BigWorld 中每个 Entity 维护的一个**属性变更事件队列**。每当实体的 `CLIENT` / `OTHER_CLIENT` / `ALL_CLIENT` 属性发生变化时，系统会生成一个带有**单调递增编号（EventNumber）**的 `HistoryEvent`，追加到队列尾部。

### 1.2 解决的核心问题

在 MMO 中，一个实体可能被多个玩家同时观察，每个玩家：
- 进入 AOI 的时间不同
- 网络带宽不同
- LOD（细节层级）不同

```
Entity A 属性变化时间线:
  ──────────────────────────────────────────────────►
  Event#1    Event#2    Event#3    Event#4    Event#5
  HP=100     HP=80      名字改了    HP=60      MP=50

玩家B：Event#1时进入AOI ─────────────────────────────►
  已收到: #1 #2 #3 #4    → 下次只需发 #5

玩家C：          Event#3时才进入AOI ──────────────────►
  已收到: #3 #4           → 下次只需发 #5

玩家D：                              刚进入AOI ──────►
  已收到: (无)            → 需要发 #5（更早的已被trim）
```

**核心思想：一个共享的事件队列 + 每个观察者一个独立游标，避免全量发送和逐观察者脏标记。**

---

## 2. 核心数据结构

### 2.1 HistoryEvent — 单条变更记录

> 源文件: `server/cellapp/history_event.hpp:25-117`

```
┌─────────────────────────────────────┐
│ HistoryEvent                        │
├─────────────────────────────────────┤
│ number_: EventNumber     (递增序号)  │
│ msgID_: MessageID        (消息类型)  │
│ msg_: char*              (序列化数据) │
│ msgLen_: int             (数据长度)   │
│ isReliable_: bool        (是否可靠)   │
│ level_: Level            (优先级/LOD) │
│ latestEventIndex_: int   (最新变更索引)│
│ pDescription_: MemberDescription*    │
└─────────────────────────────────────┘
```

**Level 是一个 union**，支持两种过滤模式：

```cpp
class Level {
    union {
        float priority_;   // 优先级模式
        int   detail_;     // LOD层级模式
    };
    bool isDetail_;

    bool shouldSend( float threshold, int detailLevel ) const {
        return isDetail_ ?
            (detailLevel <= detail_) :    // LOD: 观察者层级 ≤ 属性层级才发
            (threshold < priority_);       // 优先级: 属性优先级 > 当前阈值才发
    }
};
```

### 2.2 EventHistory — 事件队列

> 源文件: `server/cellapp/history_event.hpp:123-165`

```
┌────────────────────────────────────────────────────┐
│ EventHistory                                        │
├────────────────────────────────────────────────────┤
│ container_: list<HistoryEvent*>   (有序事件链表)      │
│ trimToEvent_: EventNumber         (可裁剪到的编号)    │
│ lastTrimmedEventNumber_: EventNumber (上次裁剪的编号) │
│ latestEventPointers_: vector<iterator> (最新变更优化) │
└────────────────────────────────────────────────────┘
```

队列示例：

```
container_:
  [Event#12] → [Event#13] → [Event#14] → [Event#15] → [Event#16]
   HP=80        名字改了       HP=60       MP=50        HP=40
     ↑                                                    ↑
   最旧                                                  最新
```

### 2.3 EntityCache — 每个观察者的游标

> 源文件: `server/cellapp/entity_cache.hpp:33-195`

每个 Witness 的 `aoiMap_` 中，为被观察的每个实体保存一个 `EntityCache`：

```
┌─────────────────────────────────────────┐
│ EntityCache  (Witness 对某实体的记录)     │
├─────────────────────────────────────────┤
│ lastEventNumber_: EventNumber           │  ← "上次发到了第几号事件"
│ lastVolatileUpdateNumber_: VolatileNumber│
│ detailLevel_: DetailLevel               │  ← 当前 LOD 级别
│ lodEventNumbers_[MAX_LOD_LEVELS]        │  ← 每个 LOD 层级的事件编号
│ priority_: Priority                     │
│ flags_: Flags (ENTER_PENDING/GONE/...)  │
└─────────────────────────────────────────┘
```

### 2.4 所有权关系

```
Entity (游戏实体)
  ├── lastEventNumber_          (全局递增计数器)
  ├── eventHistory_             (事件队列)
  └── RealEntity (权威实体)
        └── Witness (有客户端连接才有)
              ├── AoITrigger*                ← RangeTrigger 子类
              ├── aoiMap_: EntityCacheMap     ← 存储当前 AOI 内所有实体
              │     └── set<EntityCache>     ← 每个可见实体一个 Cache（含游标）
              └── entityQueue_               ← 优先级队列，用于带宽分配
```

---

## 3. 事件编号管理

> 源文件: `server/cellapp/entity.ipp:319-338`

每个 Entity 维护一个单调递增的 `lastEventNumber_`：

```cpp
EventNumber Entity::getNextEventNumber()
{
    ++lastEventNumber_;
    return lastEventNumber_;
}
```

- 初始值为 0（`entity.cpp:1273`）
- 每次属性变更时递增
- 从数据库或迁移流中恢复时会读取已有的值（`entity.cpp:1536`）

---

## 4. 事件创建与 Ghost 同步

### 4.1 本地创建

> 源文件: `server/cellapp/entity.cpp:2446-2459`

```cpp
HistoryEvent * Entity::addHistoryEventLocally(
    uint8 type, MemoryOStream & stream,
    const MemberDescription & description,
    int16 msgStreamSize, HistoryEvent::Level level )
{
    return this->eventHistory().add(
        this->getNextEventNumber(),
        type, stream, description, level, msgStreamSize );
}
```

### 4.2 同步到 Ghost

> 源文件: `server/cellapp/real_entity.cpp:703-735`

Real 实体创建事件后，立即流式发送给所有 Ghost：

```cpp
HistoryEvent * RealEntity::addHistoryEvent( ... )
{
    // 1. 加入本地历史
    HistoryEvent * pNewEvent = entity_.addHistoryEventLocally( ... );

    // 2. 发送给所有 Ghost（其他 Cell 上的影子实体）
    for (auto & haunt : haunts_)
    {
        Mercury::Bundle & bundle = haunt.bundle();
        bundle.startMessage( CellAppInterface::ghostHistoryEvent );
        bundle << entity().id();
        pNewEvent->addToStream( bundle );
    }
    return pNewEvent;
}
```

Ghost 端通过 `EventHistory::addFromStream()` 重建事件，保证所有 Cell 上的同一实体拥有相同的事件历史。

---

## 5. Witness 如何决定发送什么

### 5.1 游标比较

> 源文件: `server/cellapp/entity.cpp:2498-2510`

`Witness::update()` 每帧调用，对 `aoiMap_` 中的每个实体：

```cpp
// 从最新事件开始，向旧的方向遍历
auto eventIter = entity.eventHistory().rbegin();
auto eventEnd  = entity.eventHistory().rend();

while (eventIter != eventEnd &&
       (*eventIter)->number() > cache.lastEventNumber())
       //                       ↑ 此客户端上次收到的编号
{
    HistoryEvent & event = **eventIter;

    if (event.shouldSend( lodPriority, cache.detailLevel() ))
    {
        event.addToBundle( bundle );   // 写入网络包
    }
    ++eventIter;
}

// 更新游标到最新
cache.lastEventNumber( entity.lastEventNumber() );
```

**核心逻辑**：只遍历 `number > cache.lastEventNumber()` 的事件——即该客户端尚未收到的变更。

### 5.2 带宽控制

Witness 不一定每帧能发完所有事件。`entityQueue_` 按优先级排序，高优先级实体先发送：

```
Witness::update() 每帧:
│
├── ENTER_PENDING → sendCreate() + sendEnter()    新实体
├── GONE          → sendLeaveAoI()                离开的实体
├── 正常          → writeClientUpdateData()       属性更新
│     └── 受带宽预算限制，优先级高的先发
└── 带宽用完 → 剩余实体下帧再发
```

---

## 6. 新实体进入 AOI 的初始化

> 源文件: `server/cellapp/entity_cache.ipp:42-57`

当实体 B 刚进入玩家 A 的 AOI 时：

```cpp
void EntityCache::resetClientState()
{
    // 把 lastEventNumber 设为实体当前最新值
    lastEventNumber_ = this->pEntity()->lastEventNumber();

    // volatile 设为当前-1，确保下一帧发送位置
    lastVolatileUpdateNumber_ = this->pEntity()->volatileUpdateNumber() - 1;

    // LOD 层级设为最大（最粗糙）
    detailLevel_ = this->numLoDLevels();

    for (int i = 0; i < detailLevel_; i++)
        lodEventNumbers_[i] = 0;
}
```

**含义**：新进入的实体不会回放历史事件，而是随 `createEntity` 消息发送**当前属性快照**。`lastEventNumber_` 被设为最新值，之后只发增量。

客户端收到实体后回复 `requestEntityUpdate`，带上各 LOD 层级的事件编号：

```cpp
// witness.cpp:2025
void Witness::requestEntityUpdate( EntityID id,
    EventNumber * pEventNumbers, int size )
{
    EntityCache * pCache = aoiMap_.find( id );
    pCache->lodEventNumbers( pEventNumbers, size );
    this->addToSeen( pCache );
}
```

---

## 7. LatestChangeOnly 优化

> 源文件: `server/cellapp/history_event.cpp:194-244`

某些属性变化极其频繁（如 HP），但客户端只需要最新值。队列中积压的中间值发送毫无意义。

```cpp
// EventHistory::add()
if (description.shouldSendLatestOnly())
{
    int latestEventIndex = description.latestEventIndex();
    auto latestEventIter = latestEventPointers_[ latestEventIndex ];

    if (latestEventIter != container_.end())
    {
        // 复用旧的 HistoryEvent，用新数据覆盖
        pNewEvent = *latestEventIter;
        pNewEvent->recreate( type, eventNumber,
            stream.data(), stream.size(), level, ... );
        container_.erase( latestEventIter );
        // 重新 append 到尾部
    }
}
```

效果对比：

```
普通属性:
  [HP=100] → [HP=80] → [HP=60] → [HP=40]    4条事件

LatestChangeOnly:
  [HP=40]                                      只保留1条最新的
```

`latestEventPointers_` 按属性的 `latestEventIndex` 索引，O(1) 定位并替换。

---

## 8. LOD 与事件过滤

每个 `EntityCache` 为每个 LOD 层级记录独立的事件编号游标：

```
lodEventNumbers_[0] = 45   // LOD0（近距离详细属性）已同步到 #45
lodEventNumbers_[1] = 30   // LOD1（中距离属性）已同步到 #30
lodEventNumbers_[2] = 10   // LOD2（远距离属性）已同步到 #10
```

当实体从远处走近，LOD 层级提升（detailLevel 减小），之前未发送的高精度属性需要通过历史事件**补发**：

```
远距离 detailLevel=2，只同步 LOD2 属性
  → lodEventNumbers = [0, 0, 50]

走近后 detailLevel=0，需要同步所有属性
  → LOD0 和 LOD1 的编号为 0，从头补发这些层级的事件
```

当实体离开 AOI 时，客户端会将各 LOD 层级的事件编号发回服务端。下次重新进入 AOI 时，通过 `requestEntityUpdate` 告知服务端，实现**断点续传**。

---

## 9. 历史裁剪（Trim）

> 源文件: `server/cellapp/history_event.cpp:255-273`

事件不能无限堆积，当所有观察者都已收到某事件后即可删除：

```cpp
void EventHistory::trim()
{
    while (!container_.empty() &&
           container_.front()->number() <= trimToEvent_)
    {
        if (container_.front()->isReliable())
        {
            lastTrimmedEventNumber_ = container_.front()->number();
        }
        this->deleteEvent( container_.front() );
        container_.pop_front();
    }
    trimToEvent_ = container_.empty() ? 0 : container_.back()->number();
}
```

- 由 `CellApp::handleTrimHistoriesTimeSlice()` 周期性分时间片调用
- `lastTrimmedEventNumber_` 记录最后一个被裁剪的可靠事件编号，用于检测客户端游标是否已过期

---

## 10. 全局数据流

```
                            Entity
                              │
              ┌───────────────┼───────────────┐
              │               │               │
      lastEventNumber_    eventHistory_    propertyEventStamps_
      (全局递增计数器)    (事件队列)        (每属性时间戳)
              │               │
              │     ┌─────────┴─────────┐
              │     │   HistoryEvent     │
              │     │   #12 HP=80       │
              │     │   #13 Name="xx"   │
              │     │   #14 HP=60       │  ← LatestChangeOnly 会合并
              │     │   #15 MP=50       │
              │     └─────────┬─────────┘
              │               │
              │      ┌────────┴────────┐
              │      │                 │
        RealEntity   │           Ghost Entity
              │      │           (其他Cell)
              │  addHistoryEvent()
              │      ├── 加入本地 eventHistory_
              │      └── 流式发送 → ghostHistoryEvent → 远程 Cell
              │                          │
              │                     addFromStream()
              │                     加入 Ghost 的 eventHistory_
              │
        ┌─────┴──────────────────────────────────┐
        │          Witness                        │
        │                                         │
        │  aoiMap_:                                │
        │   ┌─ EntityCache(B): lastEvent=#12      │
        │   ├─ EntityCache(C): lastEvent=#15      │
        │   └─ EntityCache(D): lastEvent=#10      │
        │                                         │
        │  update() 每帧:                          │
        │   for cache in aoiMap_:                 │
        │     遍历 eventHistory_                   │
        │     发送 number > cache.lastEventNumber  │
        │     的事件给客户端                         │
        └─────────────────────────────────────────┘
```

---

## 11. Atlas 实现建议

### 11.1 核心设计要点

| 设计点 | BigWorld 方案 | Atlas 建议 |
|---|---|---|
| 事件存储 | `list<HistoryEvent*>` 链表 | 可考虑环形缓冲区（cache 友好） |
| 事件编号 | `int32` 单调递增 | `uint64` 避免回绕，或使用 `uint32` + 溢出处理 |
| 游标 | `EntityCache.lastEventNumber_` | 相同设计，每观察者每实体一个游标 |
| LOD 游标 | `lodEventNumbers_[MAX_LOD_LEVELS]` | 保留此设计，支持分层同步 |
| LatestChangeOnly | 复用 HistoryEvent 对象 | 保留此优化，高频属性只保留最新值 |
| 裁剪 | 周期性 trim 分时间片 | 可绑定到帧末或独立协程 |
| Ghost 同步 | 流式发送到所有 Haunt | 保留此设计 |

### 11.2 与 BigWorld 的差异点

1. **序列化格式**: BigWorld 使用自定义二进制流。Atlas 可使用 flatbuffers 或自研零拷贝方案。
2. **带宽调度**: BigWorld 通过 `entityQueue_` 优先级队列限流。Atlas 应保留此设计并考虑更细粒度的 QoS。
3. **内存管理**: BigWorld 使用 `new/delete` 分配 HistoryEvent。Atlas 建议使用对象池或 arena 分配器。
4. **可靠性**: BigWorld 区分 reliable / unreliable 事件。Atlas 应在传输层（TCP/KCP）支持此区分。

### 11.3 关键不变量

实现时必须保证以下不变量：

1. **事件编号严格递增** — `getNextEventNumber()` 必须是原子的或单线程调用
2. **游标只前进不后退** — `lastEventNumber_` 一旦更新，不能回退
3. **新进入 AOI 的实体发快照而非回放历史** — 防止历史事件积压导致带宽爆炸
4. **LatestChangeOnly 不能丢失最终值** — 合并旧事件时必须保留最新数据
5. **Ghost 的 EventHistory 与 Real 保持一致** — 编号、顺序、内容都必须相同
