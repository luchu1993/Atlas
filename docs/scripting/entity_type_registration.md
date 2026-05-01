# 实体类型注册机制

> **状态**:✅ 已落地。`TypeRegistryEmitter` + `[ModuleInitializer]` →
> `AtlasRegisterEntityType` → `EntityDefRegistry::RegisterType` 链路
> 在 `main` 上工作,DBApp 通过 `RegisterFromBinaryFile` 加载离线产出
> 的 `entity_defs.bin`(由 `Atlas.Tools.DefDump` 生成);C++ 测试
> `tests/unit/test_entity_def_registry_*.cpp` 与 C# 测试
> `Atlas.Generators.Tests/DefGeneratorTests.cs` 共同覆盖。

C++ 引擎需要每个实体类型的结构化元信息(属性、同步范围、持久化标记、
RPC 方向、Component / Struct 描述等),Atlas 由 C# 侧通过
`Atlas.Core.NativeApi.RegisterEntityType` 在启动 / 热重载后批量注入到
C++ `EntityDefRegistry`。

## 1. 引擎为什么需要它

| C++ 引擎职责 | 需要的元信息 |
|---|---|
| RPC 路由分发 | `rpc_id` → 所属类型 + 方向 |
| 属性同步范围过滤 | 属性名 → `ReplicationScope` |
| 数据库持久化 | `persistent` / `identifier` 标记 + 数据类型 |
| 空间管理 | 实体是否含 `Position` 属性 |
| 带宽优化 | 属性大小估算、delta 更新 |
| 安全校验 | 客户端发来的 `rpc_id` 合法性 + `ExposedScope` |
| 日志 / 调试 | 实体类型名、RPC 方法名 |

## 2. 注册链路

```
编译期(Atlas.Generators.Def)              运行时 / 热重载
┌─────────────────────────────┐            ┌────────────────────────────────────┐
│ 扫描 entity_defs/*.def       │            │ [ModuleInitializer]                 │
│ TypeRegistryEmitter          │ ──生成──→  │ DefEntityTypeRegistry.RegisterAll   │
│  → DefEntityTypeRegistry     │            │   ├─ 序列化 EntityTypeDescriptor    │
│  → RegisterAll()             │            │   └─ NativeApi.RegisterEntityType   │
└─────────────────────────────┘            └──────────────┬─────────────────────┘
                                                          ▼
                                           ┌────────────────────────────────────┐
                                           │ AtlasRegisterEntityType             │
                                           │   └─ EntityDefRegistry::RegisterType │
                                           └────────────────────────────────────┘
```

## 3. C++ 数据结构

`src/lib/entitydef/entity_type_descriptor.h`:

```cpp
enum class PropertyDataType : uint8_t {
  kBool, kInt8, kUInt8, kInt16, kUInt16, kInt32, kUInt32,
  kInt64, kUInt64, kFloat, kDouble, kString, kBytes,
  kVector3, kQuaternion, kCustom
};

enum class ReplicationScope : uint8_t {
  kCellPrivate = 0, kCellPublic = 1, kOwnClient = 2, kOtherClients = 3,
  kAllClients = 4, kCellPublicAndOwn = 5, kBase = 6, kBaseAndClient = 7,
};

enum class ExposedScope : uint8_t {
  kNone = 0,          // 仅服务端可调用
  kOwnClient = 1,     // 仅拥有者客户端
  kAllClients = 2,    // AoI 内任何客户端(仅 cell_methods)
};

struct PropertyDescriptor {
  std::string      name;
  PropertyDataType data_type;
  ReplicationScope scope;
  bool             persistent{false};
  bool             identifier{false};   // [Identifier]: DB 主键
  bool             reliable{false};     // 绕过 DeltaForwarder 预算,走可靠通道
  uint8_t          detail_level{5};
  uint16_t         index{0};
};

struct RpcDescriptor {
  std::string                   name;
  uint32_t                      rpc_id;  // packed: direction:2 | typeIndex:14 | method:8
  std::vector<PropertyDataType> param_types;
  ExposedScope                  exposed{ExposedScope::kNone};

  uint8_t  Direction() const;   // (rpc_id >> 22) & 0x3
  uint16_t TypeIndex() const;   // (rpc_id >> 8) & 0x3FFF
  uint8_t  MethodIndex() const; // rpc_id & 0xFF
  bool     IsExposed() const;
};

struct EntityTypeDescriptor {
  std::string                       name;
  uint16_t                          type_id;
  bool                              has_cell;
  bool                              has_client;
  std::vector<PropertyDescriptor>   properties;
  std::vector<RpcDescriptor>        rpcs;
  std::vector<EntitySlotDescriptor> slots;
};
```

`src/lib/entitydef/entity_def_registry.h` 关键 API:

| 方法 | 用途 |
|---|---|
| `static EntityDefRegistry& Instance()` | 进程级单例 |
| `bool RegisterType(const std::byte* data, int32_t len)` | 由 `AtlasRegisterEntityType` 调用;反序列化 + 建索引 |
| `Result<LoadedCounts> RegisterFromBinaryFile(path)` | DBApp 加载 `entity_defs.bin`(ATDF 容器,`Atlas.Tools.DefDump` 离线产出) |
| `const EntityTypeDescriptor* FindByName(name)` / `FindById(type_id)` | 查找 |
| `bool ValidateRpc(type_id, rpc_id)` / `const RpcDescriptor* FindRpc(rpc_id)` | RPC 校验 |
| `bool IsExposed(rpc_id)` / `ExposedScope GetExposedScope(rpc_id)` | 客户端 RPC 合法性 |
| `std::vector<const PropertyDescriptor*> GetReplicatedProperties(type_id, min_scope)` | 按范围过滤 |
| `std::vector<const PropertyDescriptor*> GetPersistentProperties(type_id)` | 持久属性列表 |
| `std::array<uint8_t,16> PersistentPropertiesDigest()` | 持久字段 MD5 摘要,用于 BaseApp ↔ DBApp schema 一致性校验 |
| `void clear()` | 热重载前清空 |

> `GetReplicatedProperties` 当前使用 `>=` 范围比较,依赖 C# 仅发送 4 个值
> (`CellPrivate=0 / BaseOnly=1 / OwnClient=2 / AllClients=3`);C++
> `ReplicationScope` 保留全部 8 个值供未来 `.def` 直接驱动时使用。

## 4. C# 侧生成

`Atlas.Generators.Def/Emitters/TypeRegistryEmitter.cs` 产出
`DefEntityTypeRegistry`(含 `[ModuleInitializer]`),在程序集加载时:

1. 为每个 `EntityTypeDescriptor` 用 `Atlas.Serialization.SpanWriter`
   构建二进制描述,字节格式与 C++ `RegisterType` 解析路径约定一致。
2. 调用
   `Atlas.Core.NativeApi.RegisterEntityType(ReadOnlySpan<byte>)`
   (内部 `[LibraryImport("atlas_engine", EntryPoint = "AtlasRegisterEntityType")]`)。
3. 热重载前由 `ClrHotReload` 触发 `AtlasUnregisterAllEntityTypes` →
   `EntityDefRegistry::clear()`;新程序集加载时 `[ModuleInitializer]`
   再次触发 `RegisterAll`。

Component / Struct 等扩展元素由 `ComponentEmitter` /
`StructEmitter` / `StructRegistryEmitter` 输出,二进制描述层与上面统一。

## 5. 各服务进程的使用

| 进程 | 使用点 |
|---|---|
| BaseApp | `INativeApiProvider::SendClientRpc / SendCellRpc / SendBaseRpc` 前用 `ValidateRpc` + `IsExposed` 校验;`GetPersistentProperties` 构造持久化快照 |
| CellApp | 属性同步按 `GetReplicatedProperties(type_id, scope)` 过滤;空间管理按 `FindByName("Position")` 判断 |
| DBApp | 共享 `EntityDefRegistry` 校验持久字段 schema;`PersistentPropertiesDigest()` 与 BaseApp 握手 |
| Reviver / LoginApp | 通常只需类型元数据,只读访问 |

## 6. 测试

- C++:`tests/unit/test_entity_def_registry_*.cpp`(注册 / 查找 / RPC /
  Component / 容器属性 / 二进制文件加载)。
- C#:`tests/csharp/Atlas.Generators.Tests/DefGeneratorTests.cs` 覆盖
  `DefEntityTypeRegistry` 的生成结果;集成测试在
  `tests/csharp/Atlas.Runtime.Tests` 验证注册路径可走通。
- 跨进程一致性:`PersistentPropertiesDigest` 在 BaseApp / DBApp 握手阶段
  比对,不一致则拒绝服务。

## 7. 关联

- [../generator/DEF_GENERATOR_DESIGN.md](../generator/DEF_GENERATOR_DESIGN.md) — `.def` 文件格式与 DefGenerator 整体设计。
- [../property_sync/PROPERTY_SYNC_DESIGN.md](../property_sync/PROPERTY_SYNC_DESIGN.md) — 属性同步与 delta 细节。
- [../property_sync/COMPONENT_DESIGN.md](../property_sync/COMPONENT_DESIGN.md) — Component 描述与同步。
- [entity_mailbox_design.md](entity_mailbox_design.md) — RPC ID 在 Mailbox 路径上的使用。
