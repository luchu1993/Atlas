# BigWorld RPC 机制参考

> 来源: BigWorld Engine 14.4.1 源码分析
> 关联: [Entity Mailbox 设计](../scripting/entity_mailbox_design.md) | [Phase 8 BaseApp](../roadmap/phase08_baseapp.md) | [Phase 10 CellApp](../roadmap/phase10_cellapp.md)
>
> 本文档聚焦 **BigWorld 原始机制**。Atlas 侧的实现以代码为准；当前仍未修复的 RPC 缺陷与待补的安全加固项见
> [Atlas RPC 缺陷与安全加固清单](../rpc/RPC_AUDIT_REMEDIATION.md)。

---

## 1. RPC 全景总览

BigWorld 实体分布在 Client / Base / Cell 三端，通过 Mailbox 实现跨端透明 RPC 调用。
所有 RPC 路径可归纳为六种方向：

```
                    ┌──────────┐
                    │  Client  │
                    └──┬───▲───┘
          entity.cell  │   │  self.ownClient / allClients / otherClients
          entity.base  │   │  self.ownClient
                       │   │
              ┌────────▼───┴────────┐
              │                     │
        ┌─────▼─────┐        ┌─────┴─────┐
        │  BaseApp   │◄──────►│  CellApp  │
        │  (Base)    │        │  (Cell)   │
        └────────────┘        └───────────┘
         self.cell ──────────► self.base
```

### 权限矩阵

| 调用方 → 目标 | 可以？ | 需要 `<Exposed/>`？ | Mercury 接口 | 方法索引类型 |
|---|---|---|---|---|
| **Client → Cell** | Yes | Yes | `cellEntityMethod`（外部） | exposed index |
| **Client → Base** | Yes | Yes | `baseEntityMethod`（外部） | exposed index |
| **Base → Cell** | Yes | No | `runScriptMethod`（内部） | internal index |
| **Cell → Base** | Yes | No | `callBaseMethod`（内部） | internal index |
| **Base → Client** | Yes | No | `ClientInterface`（直发） | exposed msg ID |
| **Cell → Client** | Yes | No | 经 Witness / BaseApp 转发 | exposed msg ID |

**核心原则: `<Exposed/>` 只管客户端 → 服务端方向。服务端之间以及服务端 → 客户端方向不受限。**

---

## 2. 实体方法定义（`.def` 文件）

BigWorld 使用 XML `.def` 文件声明实体的所有方法及其所属端：

```xml
<!-- game/res/fantasydemo/scripts/entity_defs/Avatar.def -->
<root>
    <ClientMethods>
        <showMessage>
            <Args>
                <type>   UINT8          </type>
                <source> STRING         </source>
                <message>UNICODE_STRING </message>
            </Args>
        </showMessage>
    </ClientMethods>

    <BaseMethods>
        <logOff>
            <Exposed/>                  <!-- 客户端可调 -->
        </logOff>
        <addAdmirer>                    <!-- 无 Exposed，客户端不可调 -->
            <Arg> OBJECT_ID </Arg>
        </addAdmirer>
    </BaseMethods>

    <CellMethods>
        <useItem>
            <Exposed/>                  <!-- 默认: OWN_CLIENT + ALL_CLIENTS -->
            <Arg> ITEMTYPE </Arg>
        </useItem>
        <castSpell>
            <Exposed>OWN_CLIENT</Exposed>  <!-- 仅自己的客户端可调 -->
            <Arg> SPELL_ID </Arg>
        </castSpell>
        <queryInfo>
            <Exposed>ALL_CLIENTS</Exposed> <!-- 任何客户端可调 -->
        </queryInfo>
        <internalLogic>                 <!-- 无 Exposed，客户端不可调 -->
            <Arg> INT32 </Arg>
        </internalLogic>
    </CellMethods>
</root>
```

### Exposed 标记变体

解析逻辑: `entitydef/method_description.cpp:351-384`

| `.def` 写法 | 设置的标志位 | 含义 | 适用组件 |
|---|---|---|---|
| `<Exposed/>` | `OWN_CLIENT + ALL_CLIENTS` | 任何客户端可调（默认） | Cell, Base |
| `<Exposed>OWN_CLIENT</Exposed>` | `OWN_CLIENT` | 仅实体拥有者的客户端 | Cell, Base |
| `<Exposed>ALL_CLIENTS</Exposed>` | `ALL_CLIENTS` | 任何客户端可调 | 仅 Cell |
| 无标记 | 无 | 客户端不可调 | Cell, Base |
| `<Exposed/>` on ClientMethod | **编译错误** | 客户端方法不允许 Exposed | — |

---

## 3. 六条 RPC 路径详解

### 3.1 Client → Cell（客户端调用 Cell 方法）

**前提:** 方法必须标记 `<Exposed/>`。

```
entity.cell.useItem(ITEMTYPE_SWORD)     # 客户端 Python
        │
  PyEntity.pyGet_cell() → PyServer           [client/py_entity.cpp:1030-1043]
        │
  PyServer.useItem → ServerCaller            [client/py_server.cpp:218-237]
        │  通过 EntityMethodDescriptions::find() 查找方法
        │
  ServerCaller::pyCall()                     [client/py_server.cpp:84-152]
        │  验证实体存活
        │  验证方法已 Exposed
        │
  ServerConnection::startCellEntityMessage() [connection/server_connection.cpp:892-922]
        │  exposedIndex → Mercury msgID (ExposedMethodMessageRange)
        │  写入: [MsgID][EntityID][SubMsgID?]
        │
  MethodDescription::addToStream()           [entitydef/method_description.cpp:519-528]
        │  序列化每个参数
        │
  ═══════════ Mercury UDP (外部接口) ═══════════
        │
  BaseApp: Proxy::cellEntityMethod()         [baseapp/proxy.cpp:2491-2530]
        │  嵌入 sourceEntityID (Proxy 自身 ID，不可伪造)
        │  转发到 CellApp: CellAppInterface::runExposedMethod
        │
  ═══════════ Mercury UDP (内部接口) ═══════════
        │
  CellApp: Entity::runExposedMethod()        [cellapp/entity.cpp:5399]
        │  MF_ASSERT(this->isReal())
        │
  Entity::runMethodHelper(isExposed=true)    [cellapp/entity.cpp:5418-5508]
        │  提取 sourceEntityID
        │  用 exposedMethodFromMsgID() 查找方法
        │  OWN_CLIENT 方法: 验证 id_ == sourceID
        │
  MethodDescription::callMethod()            [entitydef/method_description.cpp:814-862]
        │  反序列化参数 → Python tuple
        │  PyObject_CallObject() 执行
```

**关键源文件:**
- 客户端代理: `client/py_server.hpp/cpp` — `ServerCaller` 类
- 网络连接: `connection/server_connection.cpp:892-922` — `startCellEntityMessage()`
- 消息接口: `connection/baseapp_ext_interface.hpp:141` — `cellEntityMethod`
- CellApp 接收: `cellapp/entity.cpp:5399-5508` — `runExposedMethod()` / `runMethodHelper()`

---

### 3.2 Client → Base（客户端调用 Base 方法）

**前提:** 方法必须标记 `<Exposed/>`。

```
entity.base.logOff()                    # 客户端 Python
        │
  PyEntity.pyGet_base() → PyServer          [client/py_entity.cpp]
        │
  ServerCaller::pyCall()                    [client/py_server.cpp:108-122]
        │  isProxyCaller_ = true (区别于 cell 调用)
        │  使用 exposedIndex()
        │
  ServerConnection::startBasePlayerMessage() [connection/server_connection.cpp:841-868]
        │  消息类型: BaseAppExtInterface::baseEntityMethod
        │
  ═══════════ Mercury UDP (外部接口) ═══════════
        │
  BaseApp: Proxy::baseEntityMethod()         [baseapp/proxy.cpp:2536-2587]
        │  exposedMethodFromMsgID() 查找方法
        │  直接调用: pMethodDesc->callMethod()
        │  不需要 sourceEntityID 验证 (已通过 Proxy 认证)
```

**关键源文件:**
- 消息接口: `connection/baseapp_ext_interface.hpp:142` — `baseEntityMethod`
- BaseApp 接收: `baseapp/proxy.cpp:2536-2587` — `baseEntityMethod()`

**与 Client→Cell 的区别:**
- Base 方法直接在 BaseApp 本地执行，不需要转发
- 不需要 sourceEntityID 验证（Proxy 已绑定到唯一客户端）

---

### 3.3 Base → Cell（Base 调用 Cell 方法）

**不需要 `<Exposed/>`，可调用所有 Cell 方法。**

```
self.cell.moveTo(position)              # BaseApp Python
        │
  Base::pyGet_cell() → CellEntityMailBox     [baseapp/base.cpp:2903-2927]
        │
  CellEntityMailBox.moveTo → RemoteEntityMethod
        │                                     [entitydef/remote_entity_method.cpp:39-55]
        │
  RemoteEntityMethod::pyCall()
        │  → PyEntityMailBox::callMethod()    [entitydef/mailbox_base.cpp:141-198]
        │
  CellEntityMailBox::getStreamEx()           [baseapp/mailbox.cpp:896-902]
        │  消息类型: CellAppInterface::runScriptMethod
        │
  CommonCellEntityMailBox::getStreamCommon() [baseapp/mailbox.cpp:750-777]
        │  写入: EntityID + internalMethodIndex + 序列化参数
        │
  ═══════════ Mercury UDP (内部接口) ═══════════
        │
  CellApp: Entity::runScriptMethod()         [cellapp/entity.cpp:5241-5249]
        │  提取 uint16 methodID
        │
  Entity::runMethodHelper(isExposed=false)   [cellapp/entity.cpp:5418-5468]
        │  用 internalMethod(methodID) 查找 (不走 exposedMethod)
        │
  MethodDescription::callMethod()            → Python 执行
```

**关键源文件:**
- Mailbox 类: `baseapp/mailbox.hpp:110-133` — `CellEntityMailBox`
- 流构造: `baseapp/mailbox.cpp:750-777` — `getStreamCommon()`
- 方法查找: `baseapp/mailbox.cpp:909-912` — `findMethod()` 用 `cell().find(attr)`
- Mercury 消息: `cellapp/cellapp_interface.hpp:338-340` — `runScriptMethod` (REAL_ONLY)

**与 Client→Cell 的核心区别:**
- 使用 `internalMethod()` 索引，不使用 `exposedMethod()` 索引
- 消息走内部接口 `runScriptMethod`，不走外部接口 `cellEntityMethod`
- 不需要 `<Exposed/>` 标记

---

### 3.4 Cell → Base（Cell 调用 Base 方法）

**不需要 `<Exposed/>`，可调用所有 Base 方法。**

```
self.base.saveData()                    # CellApp Python
        │
  Entity → BaseEntityMailBox                 [cellapp/entity.cpp:5255-5299]
        │
  BaseEntityMailBox::getStream()             [cellapp/mailbox.cpp]
        │  使用 internalIndex
        │
  ═══════════ Mercury UDP (内部接口) ═══════════
        │  消息类型: BaseAppIntInterface::callBaseMethod
        │  [baseapp/baseapp_int_interface.hpp:251]
        │
  Base::callBaseMethod()                     [baseapp/base.cpp:1286-1342]
        │  提取 uint16 methodIndex
        │  用 internalMethod(index) 查找
        │
  MethodDescription::callMethod()            → Python 执行
```

**关键源文件:**
- Mailbox 类: `cellapp/mailbox.hpp:135-157` — `BaseEntityMailBox`
- Mercury 消息: `baseapp/baseapp_int_interface.hpp:251` — `callBaseMethod`
- BaseApp 处理: `baseapp/base.cpp:1286-1342` — `callBaseMethod()`

---

### 3.5 Base → Client（Base 调用客户端方法）

**不需要 `<Exposed/>`。所有 ClientMethod 均可调用。**

```
self.ownClient.showMessage(1, "sys", "Hello!")  # BaseApp Python
        │
  Proxy::pyGet_ownClient()                  [baseapp/proxy.cpp:2598]
        │  返回 ClientEntityMailBox (pClientEntityMailBox_)
        │
  ClientEntityMailBox.showMessage → RemoteClientMethod
        │                                    [baseapp/client_entity_mailbox.cpp:65-78]
        │
  RemoteClientMethod::pyCall()               [baseapp/remote_client_method.cpp:90-204]
        │  写入 proxy_.clientBundle()
        │  消息: ClientInterface 方法 + methodID + 参数
        │
  ═══════════ 直接发送到客户端 ═══════════
        │
  Client: ServerConnection::entityMethod()   [connection/server_connection.cpp:1031-1040]
        │  提取 exposedMethodID
        │
  BWEntities::handleEntityMethod()           [connection_model/bw_entities.cpp:619-643]
        │  按 EntityID 找到实体
        │
  Entity::onMethod()                         [client/entity.cpp:1140]
        │
  SimpleClientEntity::methodEvent()          [common/simple_client_entity.cpp:316-340]
        │  反序列化参数 → Python tuple
        │  MethodDescription::callMethod() → 执行客户端 Python 方法
```

**关键源文件:**
- Proxy 属性: `baseapp/proxy.cpp:2598` — `pyGet_ownClient()`
- Mailbox 类: `baseapp/client_entity_mailbox.hpp` — `ClientEntityMailBox`
- 方法调用: `baseapp/remote_client_method.cpp:90-204` — `RemoteClientMethod::pyCall()`
- 客户端接收: `common/simple_client_entity.cpp:316-340` — `methodEvent()`

---

### 3.6 Cell → Client（Cell 调用客户端方法，三种广播模式）

**不需要 `<Exposed/>`。** Cell 端提供三个属性对应不同广播范围：

| 属性 | 含义 | Python 对象 | 参数 |
|------|------|------------|------|
| `self.ownClient` / `self.client` | 仅拥有者客户端 | `PyClient(isForOwn=true, isForOthers=false)` | — |
| `self.otherClients` | AOI 内其他客户端 | `PyClient(isForOwn=false, isForOthers=true)` | — |
| `self.allClients` | 拥有者 + AOI 内所有 | `PyClient(isForOwn=true, isForOthers=true)` | — |

```
self.ownClient.showMessage(...)         # CellApp Python
        │
  Entity::pyGetAttribute("ownClient")       [cellapp/entity.cpp:5921-5947]
        │  返回 PyClient(isForOwn=true, isForOthers=false)
        │
  PyClient.showMessage → ClientCaller       [cellapp/py_client.cpp:36-81]
        │
  ClientCaller::pyCall()                    [cellapp/py_client.cpp:160-208]
        │
  Entity::sendToClient()                    [cellapp/entity.cpp:6401-6449]
        │
        ├── isForOwn: pReal_->pWitness()->sendToClient()
        │     → 通过 Witness 发给拥有者客户端
        │     → 消息经 BaseApp Proxy 转发
        │
        └── isForOthers: pReal_->addHistoryEvent()
              → 加入事件历史，供 Witness 按 AOI 分发给周围客户端
              → 每个观察者的 Witness::update() 在 tick 末尾发送
```

**Ghost 实体的处理:**

Ghost 实体不直接发送，而是转发给 Real 实体所在的 CellApp：

```cpp
// cellapp/entity.cpp:3404-3433
bool Entity::sendToClientViaReal(...) {
    int callingMode = 0;
    if (isForOwn) callingMode = MSG_FOR_OWN_CLIENT;
    if (isForOthers) callingMode |= MSG_FOR_OTHER_CLIENTS;
    this->writeClientMessageToBundle(pRealChannel_->bundle(), ...);
}
```

**关键源文件:**
- Cell 端代理: `cellapp/py_client.hpp` — `PyClient` 类
- 方法调用: `cellapp/py_client.cpp:36-81` — `ClientCaller` 类
- 发送逻辑: `cellapp/entity.cpp:6401-6449` — `sendToClient()`
- Ghost 转发: `cellapp/entity.cpp:3404-3433` — `sendToClientViaReal()`

---

## 4. 权限管理体系

BigWorld 的 RPC 权限管理是多层次的，核心目标是 **防止不可信的客户端越权调用服务端方法**。

### 4.1 权限分层架构

```
第1层  ┌─ Exposed 门禁 ─────────────┐  加载时
       │  无 Exposed → 客户端不可调   │
       │  有 Exposed → 进入第2层      │
       └─────────────────────────────┘
              │
第2层  ┌─ 参数类型安全 ──────────────┐  加载时
       │  CLIENT_UNUSABLE → 启动失败  │
       │  CLIENT_UNSAFE → 警告        │
       │  CLIENT_SAFE → 通过          │
       └─────────────────────────────┘
              │
第3层  ┌─ 消息路由 ─────────────────┐  运行时
       │  Ghost → 转发到 Real        │
       │  Real → 进入第4层           │
       └─────────────────────────────┘
              │
第4层  ┌─ SourceID 身份验证 ────────┐  运行时
       │  OWN_CLIENT: id_==source?   │
       │  ALL_CLIENTS: 不验证        │
       └─────────────────────────────┘
              │
       执行 Python 方法
```

### 4.2 第1层: Exposed 门禁

**只有 `<Exposed/>` 的 Cell/Base 方法才能被客户端调用。**

方法标志位定义 (`entitydef/method_description.hpp:282-286`):
```cpp
enum {
    IS_EXPOSED_TO_ALL_CLIENTS = 0x4,
    IS_EXPOSED_TO_OWN_CLIENT  = 0x8
};
```

引擎在加载 `.def` 文件时建立两套索引 (`entitydef/entity_method_descriptions.cpp:95-210`):
- `exposedMethods_` — 仅包含标记了 Exposed 的方法，客户端使用此索引
- 全部方法列表 — 服务端内部使用 `internalMethod()` 访问

### 4.3 第2层: 参数类型安全

加载时检查 Exposed 方法的参数类型 (`entitydef/entity_method_descriptions.cpp:161-182`):

| 安全级别 | 结果 | 示例 |
|---|---|---|
| `CLIENT_SAFE` | 正常通过 | INT32, STRING, FLOAT, VECTOR3 |
| `CLIENT_UNSAFE` | **警告** (潜在安全漏洞) | PYTHON 类型参数 |
| `CLIENT_UNUSABLE` | **拒绝注册，启动失败** | MAILBOX 类型参数 |

```cpp
// entitydef/entity_method_descriptions.cpp:167-181
if (clientSafety & DataType::CLIENT_UNSAFE) {
    WARNING_MSG("%s.%s is an Exposed method that takes a "
                "PYTHON arg (potential security hole)\n", ...);
}
if (clientSafety & DataType::CLIENT_UNUSABLE) {
    ERROR_MSG("%s.%s is an Exposed method that takes a "
              "MAILBOX arg (type cannot be sent to/from client)\n", ...);
    methodsOk = false;  // 启动失败
}
```

### 4.4 第3层: REAL_ONLY 消息路由

Exposed 方法的处理消息被标记为 `REAL_ONLY`:

```cpp
// cellapp/cellapp_interface.hpp:372
MF_VARLEN_ENTITY_MSG( runExposedMethod, REAL_ONLY )
```

如果客户端的 RPC 消息到达 Ghost 实体，消息处理器会自动转发到 Real 实体所在的 CellApp
(`cellapp/message_handlers.cpp:184-197`)，不会在 Ghost 上执行。

### 4.5 第4层: SourceEntityID 身份验证

这是最关键的运行时安全机制。

**消息构造时 (BaseApp 端):**
```cpp
// baseapp/proxy.cpp:2527
// Proxy 把自己的 entityID 嵌入消息
// 由 BaseApp 写入，客户端无法伪造
b << id_;
```

**CellApp 验证时:**
```cpp
// cellapp/entity.cpp:5431-5457
if (isExposed) {
    data >> sourceID;    // 从消息中取出 sourceEntityID

    pMethodDescription = pEntityType_->description().cell()
        .exposedMethodFromMsgID(methodID, data, range);

    // OWN_CLIENT 方法: 验证调用者身份
    if (pMethodDescription->isExposedToOwnClientOnly()) {
        if (id_ != sourceID) {
            WARNING_MSG("Blocked method %s on entity %u from client %u. "
                        "Method is exposed as OWN_CLIENT.\n", ...);
            return;    // 拒绝执行
        }
    }
}
```

**安全保障:**
- `sourceID` 由 BaseApp (Proxy) 写入，不是客户端自己填的，客户端无法伪造
- `OWN_CLIENT` 方法只允许实体拥有者调用
- `ALL_CLIENTS` 方法不验证调用者（设计如此）

### 4.6 不存在的机制

| 机制 | 状态 | 说明 |
|---|---|---|
| 调用频率限制 | **无** | Exposed 方法无 rate limiting |
| ACL / 角色权限 | **无** | 无更细粒度的权限系统 |
| IP 白名单 | **无** | 方法级别无此机制 |

---

## 5. 客户端可达性约束

权限体系之外，BigWorld 还有一层**架构级约束**：客户端能向哪些实体发送 RPC，取决于网络拓扑本身。

### 5.1 客户端四条路径的可达性

`connection/server_connection.cpp:928-948` 中的 `startServerEntityMessage()` 是客户端所有 RPC 的出口，逻辑一目了然：

```cpp
BinaryOStream * ServerConnection::startServerEntityMessage(
    int methodID, EntityID entityID, bool isForBaseEntity)
{
    if (entityID == id_)              // 目标是 Player 自己
    {
        if (isForBaseEntity)
            return &this->startBasePlayerMessage(methodID);   // → 自己的 Base
        else
            return &this->startCellPlayerMessage(methodID);   // → 自己的 Cell
    }
    else if (!isForBaseEntity)        // 目标是其他实体的 Cell
    {
        return &this->startCellEntityMessage(methodID, entityID);  // → 允许
    }
    else                              // 目标是其他实体的 Base
    {
        ERROR_MSG("Can only call base methods on the connected entity\n");
        return NULL;                  // → 直接拒绝
    }
}
```

| 路径 | 目标 | 是否允许 | 函数 |
|---|---|---|---|
| `self.base.method()` | 自己的 Base | 允许 | `startBasePlayerMessage()` |
| `self.cell.method()` | 自己的 Cell | 允许 | `startCellPlayerMessage()` |
| `other.cell.method()` | 其他实体的 Cell | 允许（需 `ALL_CLIENTS`） | `startCellEntityMessage(entityID)` |
| `other.base.method()` | 其他实体的 Base | **拒绝** | `ERROR_MSG + return NULL` |

### 5.2 三重拦截机制

客户端调用其他实体的 Base 方法被三层独立检查拦截：

**第1层 — 客户端 Python 属性访问** (`client/py_entity.cpp:1074-1079`):

```cpp
PyObject * PyEntity::pyGet_base() {
    if (!pEntity_->isPlayer()) {
        PyErr_SetString(PyExc_TypeError,
            "Entity.base is only available on the connected entity");
        return NULL;      // 非 Player 实体连 .base 属性都拿不到
    }
    // ...
}
```

**第2层 — 客户端 C++ 方法调用** (`client/py_server.cpp:108-113`):

```cpp
// ServerCaller::pyCall()
if (isProxyCaller_ && !pEntity->isPlayer()) {
    PyErr_SetString(PyExc_TypeError,
        "Can only call base methods on the player");
    return NULL;
}
```

**第3层 — 网络发送** (`connection/server_connection.cpp:947-948`):

```cpp
ERROR_MSG("Can only call base methods on the connected entity\n");
return NULL;
```

### 5.3 为什么 Cell 可以而 Base 不行？

这是网络架构决定的：

```
Client ──── 唯一连接 ────► BaseApp (Proxy = 自己的 Base)
                               │
                               │ 转发 (消息携带目标 entityID)
                               ▼
                            CellApp
                           ┌───────────────────┐
                           │ Entity A (Player)  │
                           │ Entity B (NPC)     │ ← ALL_CLIENTS 方法可被任何客户端调用
                           │ Entity C (其他玩家)│
                           └───────────────────┘
```

- **Cell 方法：** 消息经 Proxy 转发到 CellApp，消息体中包含目标 `entityID`（`proxy.cpp:2506` 读取）。CellApp 管理空间中所有实体，所以能路由到 AOI 内任意实体。
- **Base 方法：** 消息直接在 Proxy 上执行（`proxy.cpp:2580` 的 `callMethod(this, ...)`），没有 EntityID 路由。其他玩家的 Base 可能在另一个 BaseApp 进程上，客户端到 BaseApp 的连接是一对一绑定的，根本没有转发路径。

### 5.4 `.def` 解析也禁止 Base 使用 ALL_CLIENTS

`entitydef/method_description.cpp:363`:

```cpp
if (component == CELL && exposedFlag == "ALL_CLIENTS")
{
    this->setExposedToAllClients();    // 仅 Cell 方法允许
}
```

Base 方法写 `<Exposed>ALL_CLIENTS</Exposed>` 会触发解析错误（line 377-382）。
这与架构约束一致 —— 即使允许了也没有网络路径可达。

### 5.5 跨实体查询的标准模式

虽然客户端不能直接调用其他实体的 Base 方法，但 BigWorld 提供了标准的间接模式：

**模式1: 通过自己的 Base 中转**

```python
# 客户端
self.base.queryPlayerInfo(otherEntityID)     # 调用自己的 Base

# BaseApp 端
def queryPlayerInfo(self, targetID):
    otherEntity = BigWorld.entities[targetID] # Base 端可直接访问同进程实体
    self.client.onPlayerInfo(otherEntity.name)# 结果回调给客户端
```

**模式2: 通过 Cell 的 ALL_CLIENTS 方法**

```python
# 客户端（otherEntity 在 AOI 内）
otherEntity.cell.queryInfo()                 # 需要 <Exposed>ALL_CLIENTS</Exposed>

# CellApp 端
def queryInfo(self, sourceEntityID):
    caller = BigWorld.entities[sourceEntityID]
    caller.client.onQueryResult(self.info)   # 通过调用者的客户端回调
```

**核心设计思想：** 客户端是不可信的，不应直接操作其他玩家的 Base 实体。所有跨实体交互都必须经服务端逻辑中转，由服务端决定暴露哪些信息。

---

## 6. Ghost 实体的 RPC 处理

当 Space 跨多个 CellApp 时，实体在一个 CellApp 上是 **Real**（权威），在相邻 CellApp 上有 **Ghost**（只读副本）。
Ghost 的存在对脚本层完全透明 —— RPC 在引擎层自动路由到 Real 执行。

### 6.1 Real / Ghost 的基本关系

```
CellApp A                          CellApp B
┌──────────────────┐              ┌──────────────────┐
│ Entity X (Real)  │              │ Entity X (Ghost) │
│  pReal_ ≠ NULL   │◄─ channel ──│  pRealChannel_   │
│  haunts_[] → B   │              │  pReal_ = NULL   │
└──────────────────┘              └──────────────────┘
```

判定方式（`entity.ipp:110-127`）：
- `isReal()` → `pReal_ != NULL`
- Ghost 持有 `pRealChannel_` 指向 Real 所在的 CellApp
- Real 持有 `haunts_[]` 列表追踪所有 Ghost 的位置（`real_entity.hpp:76-100`）

### 6.2 消息级 Reality 标记

`.def` 文件 **没有** 针对 RPC 方法的 Ghost 配置。Ghost 的 RPC 路由由 `cellapp_interface.hpp`
中的**消息级 Reality 标记**硬编码控制：

```cpp
// cellapp_interface.hpp
MF_RAW_VARLEN_ENTITY_MSG( runScriptMethod,  REAL_ONLY )   // line 339, Base→Cell
MF_VARLEN_ENTITY_MSG( runExposedMethod,     REAL_ONLY )   // line 372, Client→Cell
```

Reality 等级定义（`cellapp_interface.hpp:32-37`）：

| Reality | 含义 | 到达 Ghost 时 |
|---|---|---|
| `REAL_ONLY` | 仅在 Real 上执行 | 自动转发到 Real |
| `GHOST_ONLY` | 仅在 Ghost 上执行 | 直接执行 |
| `GHOST_AND_REAL` | 两者都执行 | 直接执行 |
| `WITNESS_ONLY` | 仅有 Witness 的实体 | 需要观察者存在 |

### 6.3 Ghost 收到 REAL_ONLY 消息时的转发

`message_handlers.cpp:183-206` 中的处理逻辑：

```
Ghost CellApp 收到 runExposedMethod / runScriptMethod
        │
  EntityMessageHandler::handleMessage()
        │  检查: reality_ >= REAL_ONLY && !pEntity->isReal()
        │
        ├── pEntity 存在（本地有 Ghost）
        │     └── pEntity->pRealChannel() 获取 Real 的 Channel
        │
        └── pEntity 不存在
              └── Entity::population().findRealChannel(entityID)
        │
  Entity::forwardMessageToReal()          [entity.cpp:2268-2294]
        │  原封不动转发到 Real 所在的 CellApp
        │
  ════════════ 内部 Mercury UDP ════════════
        │
  Real CellApp 收到消息 → 正常执行
```

转发实现（`entity.cpp:2268-2294`）：

```cpp
void Entity::forwardMessageToReal(
    CellAppChannel & realChannel, EntityID entityID,
    uint8 messageID, BinaryIStream & data,
    const Mercury::Address & srcAddr, Mercury::ReplyID replyID)
{
    Mercury::Bundle & bundle = realChannel.channel().bundle();
    const Mercury::InterfaceElement & ie =
        CellAppInterface::gMinder.interfaceElement(messageID);

    if (replyID == Mercury::REPLY_ID_NONE)
        bundle.startMessage(ie);
    else
        bundle.startRequest(ie, new ReplyForwarder(srcAddr, replyID));

    bundle << entityID;
    bundle.transfer(data, data.remainingLength());
}
```

**所有 RPC（无论来自客户端还是 Base）在 Ghost 上都是透明转发，脚本层完全无感。**

### 6.4 Ghost 专有消息（GHOST_ONLY）

Real 通过一组 GHOST_ONLY 消息主动将状态推送到 Ghost（`cellapp_interface.hpp:295-336`）：

| 消息 | 用途 |
|---|---|
| `onload` | Ghost 初始化（从流中恢复） |
| `ghostPositionUpdate` | 位置同步 |
| `ghostHistoryEvent` | 事件历史同步（供 Ghost 上的 Witness 生成客户端更新） |
| `ghostSetReal` / `ghostSetNextReal` | 通知 Ghost 其 Real 的位置 / 迁移目标 |
| `delGhost` | 删除 Ghost |
| `ghostVolatileInfo` | 位置/朝向等 Volatile 信息同步 |
| `ghostControllerCreate/Delete/Update` | Controller 状态同步 |
| `ghostedDataUpdate` | **属性值同步**（只同步带 GHOSTED 标记的属性） |

### 6.5 Ghost 上的客户端 RPC 发送

Ghost 实体调用 `self.otherClients.method()` 时不直接发送，而是通过 Real 中转
（`entity.cpp:3404-3433`）：

```
Ghost Entity: self.otherClients.showEffect("slash")
        │
  Entity::sendToClientViaReal()           [entity.cpp:3404]
        │  MF_ASSERT(!this->isReal())
        │
  writeClientMessageToBundle(pRealChannel_->bundle(), ...)
        │  通过 Ghost→Real 通道发给 Real 所在的 CellApp
        │
  ════════════ 内部 Mercury ════════════
        │
  Real CellApp 接收 → 通过 Witness 分发给客户端
```

### 6.6 `.def` 中与 Ghost 相关的配置：属性 Flags

虽然 RPC 方法没有 Ghost 配置，但**属性**通过 `<Flags>` 标签控制是否同步到 Ghost。

属性标志位定义（`entitydef/data_description.hpp:30-41`）：

```cpp
enum EntityDataFlags {
    DATA_GHOSTED        = 0x01,   // 同步到 Ghost
    DATA_OTHER_CLIENT   = 0x02,   // 发送给其他客户端
    DATA_OWN_CLIENT     = 0x04,   // 发送给拥有者客户端
    DATA_BASE           = 0x08,   // 发送给 Base
};
```

在 `.def` 文件中的使用（`data_description.cpp:72-83`）：

```xml
<Properties>
    <!-- CELL_PRIVATE: 不同步到 Ghost，不发给任何客户端 -->
    <secretData>
        <Type> INT32 </Type>
        <Flags> CELL_PRIVATE </Flags>          <!-- flags = 0 -->
    </secretData>

    <!-- CELL_PUBLIC: 同步到 Ghost，但不发给客户端 -->
    <aiState>
        <Type> INT32 </Type>
        <Flags> CELL_PUBLIC </Flags>           <!-- flags = GHOSTED -->
    </aiState>

    <!-- OTHER_CLIENTS: 同步到 Ghost + 发给其他客户端 -->
    <modelName>
        <Type> STRING </Type>
        <Flags> OTHER_CLIENTS </Flags>         <!-- flags = GHOSTED | OTHER_CLIENT -->
    </modelName>

    <!-- ALL_CLIENTS: 同步到 Ghost + 发给所有客户端 -->
    <hp>
        <Type> INT32 </Type>
        <Flags> ALL_CLIENTS </Flags>           <!-- flags = GHOSTED | OTHER_CLIENT | OWN_CLIENT -->
    </hp>

    <!-- OWN_CLIENT: 仅发给拥有者客户端，不同步到 Ghost -->
    <inventory>
        <Type> ARRAY <of> INT32 </of> </Type>
        <Flags> OWN_CLIENT </Flags>            <!-- flags = OWN_CLIENT -->
    </inventory>

    <!-- BASE_AND_CLIENT: 发给 Base + 拥有者客户端，不同步到 Ghost -->
    <password>
        <Type> STRING </Type>
        <Flags> BASE_AND_CLIENT </Flags>       <!-- flags = OWN_CLIENT | BASE -->
    </password>
</Properties>
```

完整 Flags 映射表：

| Flags 标签 | GHOSTED | OTHER_CLIENT | OWN_CLIENT | BASE | 典型用途 |
|---|---|---|---|---|---|
| `CELL_PRIVATE` | - | - | - | - | AI 内部状态 |
| `CELL_PUBLIC` | Yes | - | - | - | 跨 Cell 可见但客户端不可见 |
| `OTHER_CLIENTS` | Yes | Yes | - | - | 模型名、特效等其他玩家可见属性 |
| `OWN_CLIENT` | - | - | Yes | - | 背包、技能栏等私有数据 |
| `ALL_CLIENTS` | Yes | Yes | Yes | - | 血量、名字等全员可见属性 |
| `CELL_PUBLIC_AND_OWN` | Yes | - | Yes | - | Ghost 可见 + 拥有者客户端 |
| `BASE` | - | - | - | Yes | 仅 Base 端数据 |
| `BASE_AND_CLIENT` | - | - | Yes | Yes | Base + 拥有者客户端 |

**设计规律：需要其他客户端看到的属性必然需要 GHOSTED，因为其他客户端的 Witness 可能运行在
Ghost 所在的 CellApp 上，需要读取 Ghost 的属性数据来生成客户端更新。**

### 6.7 属性的 DetailLevel（LoD）

属性还可以配置 `<DetailLevel>` 实现距离相关的 Ghost 属性过滤（`data_lod_level.hpp`）：

```xml
<LoDLevels>
    <level> 20  <hyst>4</hyst>  <label>NEAR</label>   </level>
    <level> 100 <hyst>10</hyst> <label>MEDIUM</label>  </level>
    <level> 250 <hyst>20</hyst> <label>FAR</label>     </level>
</LoDLevels>

<Properties>
    <detailedModel>
        <Type> STRING </Type>
        <Flags> OTHER_CLIENTS </Flags>
        <DetailLevel> NEAR </DetailLevel>       <!-- 近距离才同步 -->
    </detailedModel>
</Properties>
```

### 6.8 Controller 的 Ghost 运行域

Controller 有三种运行域（`controller.hpp:28-33`）：

```cpp
enum ControllerDomain {
    DOMAIN_GHOST          = 1,     // 仅在 Ghost 上
    DOMAIN_REAL           = 2,     // 仅在 Real 上
    DOMAIN_GHOST_AND_REAL = 3      // 两者都运行
};
```

Real 通过 `ghostControllerCreate/Delete/Update` 消息将 Controller 状态同步到 Ghost。
Controller 子类在代码中硬编码 `domain()` 返回值，`.def` 中无配置。

---

## 7. 方法索引体系

BigWorld 维护两套独立的方法索引，这是客户端和服务端使用不同调用路径的根基：

### 7.1 Exposed Index（暴露索引）

- 仅包含标记了 `<Exposed/>` 的方法
- 客户端使用此索引定位方法
- 通过 `ExposedMethodMessageRange` 映射为 Mercury 消息 ID
- 方法按 `ExposedMethodsSortHelper` 排序优化消息编码

```cpp
// entitydef/entity_method_descriptions.cpp:95-97
// 收集 Exposed 方法
if (mdesc.isExposed()) {
    exposedMethods_.push_back(&mdesc);
}

// entitydef/entity_method_descriptions.cpp:205-211
// 分配消息 ID
for (size_t i = 0; i < exposedMethods_.size(); ++i) {
    exposedMethods_[i]->setExposedMsgID(startMsgID + i);
}
```

### 7.2 Internal Index（内部索引）

- 包含所有方法（不论是否 Exposed）
- 服务端内部调用（Base→Cell, Cell→Base）使用此索引
- `internalMethod(uint16_t index)` 直接按数组偏移查找

### 7.3 索引使用场景

| 路径 | 发送端使用 | 接收端查找 |
|---|---|---|
| Client → Cell | `exposedIndex()` | `exposedMethodFromMsgID()` |
| Client → Base | `exposedIndex()` | `exposedMethodFromMsgID()` |
| Base → Cell | `internalIndex()` | `internalMethod(index)` |
| Cell → Base | `internalIndex()` | `internalMethod(index)` |
| Server → Client | `exposedMsgID()` | `exposedMethod(methodID)` |

---

## 8. 网络消息协议

### 8.1 外部接口（客户端 ↔ BaseApp）

定义在 `connection/baseapp_ext_interface.hpp`:

```cpp
MF_METHOD_RANGE_MSG( cellEntityMethod, 2 )   // line 141, 客户端 → Cell
MF_METHOD_RANGE_MSG( baseEntityMethod, 1 )   // line 142, 客户端 → Base
```

消息 ID 编码使用 `ExposedMethodMessageRange` (`network/exposed_message_range.hpp`):
- 小 ID 空间: 一级消息 ID 直接映射
- 大 ID 空间: 一级消息 ID + 二级 `subMsgID (uint8)` 扩展

### 8.2 内部接口（服务端间）

CellApp 接口 (`cellapp/cellapp_interface.hpp`):
```cpp
MF_RAW_VARLEN_ENTITY_MSG( runScriptMethod, REAL_ONLY )   // line 338, Base → Cell
MF_VARLEN_ENTITY_MSG( runExposedMethod, REAL_ONLY )      // line 372, Client → Cell (经 Base 转发)
```

BaseApp 内部接口 (`baseapp/baseapp_int_interface.hpp`):
```cpp
MF_BASE_STREAM_MSG_EX( callBaseMethod )     // line 251, Cell → Base
MF_BASE_STREAM_MSG_EX( callCellMethod )     // line 253, 外部 → Cell (经 Base 转发)
```

客户端接口 (`connection/client_interface.hpp`):
```cpp
MF_CALLBACK_MSG( entityMethod )             // Server → Client
MERCURY_METHOD_RANGE_MSG( entityMethod, 2 ) // 方法消息范围
```

---

## 9. 关键类参考

### 9.1 客户端侧

| 类 | 文件 | 职责 |
|---|---|---|
| `PyServer` | `client/py_server.hpp/cpp` | `entity.cell` / `entity.base` 代理对象 |
| `ServerCaller` | `client/py_server.cpp:25-71` | 方法调用包装，序列化参数并发送 |
| `ServerConnection` | `connection/server_connection.hpp/cpp` | 管理客户端到服务端的网络连接 |

### 9.2 服务端侧 — BaseApp

| 类 | 文件 | 职责 |
|---|---|---|
| `Base` | `baseapp/base.hpp/cpp` | Base 实体，持有 Python 对象 |
| `Proxy` | `baseapp/proxy.hpp/cpp` | Base + 客户端连接管理 |
| `CellEntityMailBox` | `baseapp/mailbox.hpp:110-133` | Base → Cell RPC 的 Mailbox |
| `ClientEntityMailBox` | `baseapp/client_entity_mailbox.hpp` | Base → Client RPC 的 Mailbox |
| `RemoteClientMethod` | `baseapp/remote_client_method.hpp/cpp` | Base→Client 方法调用包装 |

### 9.3 服务端侧 — CellApp

| 类 | 文件 | 职责 |
|---|---|---|
| `Entity` | `cellapp/entity.hpp/cpp` | Cell 实体，~7800 LOC |
| `PyClient` | `cellapp/py_client.hpp/cpp` | `self.ownClient/otherClients/allClients` 代理 |
| `ClientCaller` | `cellapp/py_client.cpp:36-81` | Cell→Client 方法调用包装 |
| `BaseEntityMailBox` | `cellapp/mailbox.hpp:135-157` | Cell → Base RPC 的 Mailbox |

### 9.4 公共基础

| 类 | 文件 | 职责 |
|---|---|---|
| `MethodDescription` | `entitydef/method_description.hpp/cpp` | 方法元数据：参数、序列化、调用 |
| `EntityMethodDescriptions` | `entitydef/entity_method_descriptions.hpp/cpp` | 方法集合管理，暴露索引 |
| `PyEntityMailBox` | `entitydef/mailbox_base.hpp/cpp` | Mailbox 基类，通用的 `callMethod()` |
| `RemoteEntityMethod` | `entitydef/remote_entity_method.hpp/cpp` | 远程方法调用的 Python 包装 |
| `ExposedMethodMessageRange` | `network/exposed_message_range.hpp` | 暴露方法 → Mercury 消息 ID 映射 |

---

## 10. MethodDescription 核心方法参考

以下是 `MethodDescription` 中与 RPC 序列化/调用直接相关的关键方法:

| 方法 | 位置 | 用途 |
|---|---|---|
| `addToStream(source, stream)` | cpp:519-528 | 服务端间调用序列化（仅参数） |
| `addToServerStream(source, stream, sourceEntityID)` | cpp:547-558 | 含源实体 ID 的序列化（Exposed Cell 方法） |
| `addToClientStream(source, stream, entityID)` | cpp:577-599 | 服务端→客户端序列化（含 sub-message ID） |
| `callMethod(self, data, sourceID)` | cpp:814-862 | 反序列化参数 + 调用 Python 方法 |
| `getArgsAsTuple(data, sourceID)` | cpp:827 | 从二进制流构建 Python 参数 tuple |
| `extractSourceEntityID(args, sourceID)` | cpp:612-659 | 从参数中提取源实体 ID |
| `areValidArgs(exposed, args, genException)` | cpp:487-503 | 参数合法性校验 |
| `clientSafety()` | cpp:1310-1312 | 检查参数类型的客户端安全级别 |

---

## 11. 附录: 完整调用链路对比表

| 步骤 | Client→Cell | Client→Base | Base→Cell | Cell→Base | Base→Client | Cell→Client |
|------|---|---|---|---|---|---|
| 1. Python 入口 | `entity.cell.m()` | `entity.base.m()` | `self.cell.m()` | `self.base.m()` | `self.ownClient.m()` | `self.ownClient.m()` |
| 2. 代理对象 | `PyServer` | `PyServer` | `CellEntityMailBox` | `BaseEntityMailBox` | `ClientEntityMailBox` | `PyClient` |
| 3. 方法包装 | `ServerCaller` | `ServerCaller` | `RemoteEntityMethod` | `RemoteEntityMethod` | `RemoteClientMethod` | `ClientCaller` |
| 4. 索引类型 | exposed | exposed | internal | internal | exposedMsgID | exposedMsgID |
| 5. Mercury 消息 | `cellEntityMethod` | `baseEntityMethod` | `runScriptMethod` | `callBaseMethod` | `ClientInterface` | 经 Witness |
| 6. 网络接口 | 外部 | 外部 | 内部 | 内部 | 直发客户端 | 内部→BaseApp→客户端 |
| 7. 需要 Exposed? | Yes | Yes | No | No | No | No |
| 8. sourceID 验证 | OWN_CLIENT 时 | No | No | No | N/A | N/A |

> Atlas 侧仍未修复的缺陷与安全加固待办见 [Atlas RPC 缺陷与安全加固清单](../rpc/RPC_AUDIT_REMEDIATION.md)。
