# DefGenerator: 进程感知代码生成 + 客户端 RPC 安全校验

> 状态：已实现
> 关联：[BigWorld RPC 参考](../bigworld_ref/BIGWORLD_RPC_REFERENCE.md) · [Entity Mailbox](../scripting/entity_mailbox_design.md) · [Property Sync](../property_sync/property_sync_design.md) · [Component Design](../property_sync/component_design.md)

`.def` 文件是 Atlas 实体的唯一真相来源。`Atlas.Generators.Def` 是仓库中唯一的实体相关 Source Generator，负责把 `.def` 加用户的 `[Entity("Name")] partial class` 编织成所有进程的实体代码（属性、序列化、RPC、Mailbox、Dispatcher、Component、TypeRegistry）。C++ 侧不解析 `.def`，而是消费 Generator 在 `[ModuleInitializer]` 时刻发往 native 的二进制描述符（`ATDF` 文件格式）。

---

## 1. .def 文件

每个实体类型一个 `.def`。组件类型可以单独建文件（`<component>` 根）也可以内联在实体里。

```xml
<entity name="Avatar">

  <types>
    <struct name="ItemStack">
      <field name="id"    type="int32" />
      <field name="count" type="int32" />
    </struct>
    <alias name="GoldAmount" type="int32" />
  </types>

  <properties>
    <property name="hp"      type="int32"   scope="all_clients"     persistent="true" reliable="true" />
    <property name="mainWeapon" type="ItemStack" scope="all_clients" persistent="true" />
    <property name="bag"     type="list[ItemStack]" scope="own_client" persistent="true" />
    <property name="cooldowns" type="dict[int32,float]" scope="own_client" />
    <property name="gold"    type="GoldAmount" scope="base"          persistent="true" />
  </properties>

  <client_methods>
    <method name="ShowDamage">
      <arg name="amount"     type="int32" />
      <arg name="attackerId" type="uint32" />
    </method>
  </client_methods>

  <cell_methods>
    <method name="CastSkill" exposed="own_client">
      <arg name="skillId" type="int32" />
    </method>
    <method name="QueryNearby" reply="list[uint32]">
      <arg name="radius" type="float" />
    </method>
  </cell_methods>

  <base_methods>
    <method name="UseItem" exposed="own_client">
      <arg name="itemId" type="int32" />
    </method>
  </base_methods>

  <components>
    <component name="combat" type="CombatComponent" scope="all_clients" />
    <component name="inventory" type="InventoryComponent" scope="own_client" lazy="true" />
    <component name="ai" type="AIComponent" local="server" />
  </components>

</entity>
```

### 1.1 属性 scope（8 值，对齐 BigWorld Flags）

| scope | Ghost | Owner | Other | Base | 典型用途 |
|---|---|---|---|---|---|
| `cell_private` | – | – | – | – | AI 内部状态 |
| `cell_public` | ✓ | – | – | – | 跨 Cell 可见、客户端不可见 |
| `own_client` | – | ✓ | – | – | 背包、技能冷却 |
| `other_clients` | ✓ | – | ✓ | – | 模型、装备外观 |
| `all_clients` | ✓ | ✓ | ✓ | – | 血量、名字 |
| `cell_public_and_own` | ✓ | ✓ | – | – | Ghost 可见 + owner 客户端 |
| `base` | – | – | – | ✓ | 仅 Base 端 |
| `base_and_client` | – | ✓ | – | ✓ | Base + owner 客户端 |

`scope="all_clients"` 等含 Ghost 同步的取值意味着属性会通过 cell 之间的 ghost 通道复制 —— BigWorld 的设计规律：Other 客户端可见 ⊆ Ghost 同步必需。

### 1.2 RPC 标记

| 属性 | 取值 | 适用 | 说明 |
|---|---|---|---|
| `exposed` | `own_client` / `all_clients` / `true` | cell_methods, base_methods | 客户端是否可调；`true` ≡ `own_client`（比 BigWorld 默认更严） |
| `reply` | 类型字串 | cell_methods, base_methods | 声明 RPC 有应答；省略 = fire-and-forget |
| `reliable` | `true` / `false` | property | 改值走可靠通道，绕过 DeltaForwarder 字节预算 |
| `persistent` | `true` / `false` | property | 进入持久化集 |

`client_methods` 不允许 `exposed`、`reply`，由 generator 报错（DEF002 / DEF018）。`base_methods` 不允许 `exposed="all_clients"`（架构禁止跨实体 base RPC，DEF003）。

### 1.3 类型表达式

`type` 字段除标量外支持递归表达式，由 `DefTypeExprParser` 解析：

| 表达式 | 含义 |
|---|---|
| `int32` / `string` / `vector3` / … | 标量 |
| `MyStruct` | 引用 `<types>` 中的 struct 或 alias |
| `list[T]` | 列表，元素可继续嵌套；运行时支撑类型 `Atlas.Observable.ObservableList<T>` |
| `dict[K,V]` | K 必须是标量；运行时 `Atlas.Observable.ObservableDict<K,V>` |

容器属性默认 `max_size="4096"`。Struct 属性的 setter 通过生成的 `<Prop>MutRef` 引用类暴露字段级变更（避免 CS1612），同时驱动 dirty 位。

### 1.4 Components

`<components>` 内每个 `<component>` 是一个槽：`name=` 是 C# 槽名（脚本访问入口），`type=` 是组件类名。`scope` 沿用 8 值表，`local="server" | "client"` 切换为本进程私有（无网络存在），`lazy="true"` 推迟实例化。组件类型也可独立成 `<component>` 根 `.def` 文件并 `extends="..."` 形成单继承链，由 `DefLinker` 跨文件解析。

属性 scope 必须 ⊆ 组件 scope（observer 子集关系）。组件 RPC 也走 cell/base/client 三段式分类，`rpc_id` 高 8 位放 `slot_idx` 区分入口。

### 1.5 ATLAS_DEF008：保留属性名 `position`

Position 走专门的 volatile 通道（envelope `kEntityPositionUpdate` / 0xF001），由 `ClientEntity` 基类的 `ApplyPositionUpdate` 接管。如果 `.def` 把 `position` 声明为可复制属性，`DefParser` 标记 `IsReservedPosition=true`，所有 emitter 把它当作不存在 —— 否则 volatile 通道与 ApplyReplicatedDelta 双重投递。Generator 同时报 DEF008 警告。

---

## 2. 用户视角

服务端的 base / cell 共用同一个 `ServerEntity` 基类，进程上下文由编译宏切；客户端走单独的 `Atlas.Client.ClientEntity`。

```csharp
// 服务端（CellApp 项目，<DefineConstants>ATLAS_CELL</DefineConstants>）
[Entity("Avatar")]
public partial class Avatar : ServerEntity
{
    public partial void CastSkill(int skillId)
    {
        Hp -= 50;                                  // 自动 dirty + OnHpChanged
        AllClients.ShowDamage(50, EntityId);       // generator 发送 stub
        if (Hp <= 0) Base.OnPlayerDead();
    }

    partial void OnHpChanged(int oldValue, int newValue)
    {
        if (newValue <= 0) Die();
    }
}

// 客户端（Unity / Desktop / NetClientDemo，<DefineConstants>ATLAS_CLIENT</DefineConstants>）
[Entity("Avatar")]
public partial class Avatar : ClientEntity
{
    public partial void ShowDamage(int amount, uint attackerId)
    {
        // 收到 RPC，刷 UI
    }

    protected internal override void OnEnterWorld()
    {
        // 初始快照已应用，OnXxxChanged 不会因初始值触发 — 在这里做整体初始化
    }
}
```

服务端单一 `ServerEntity` 基类的取舍：base 与 cell 的差异完全由属性 scope 与 RPC 矩阵驱动 + `ATLAS_BASE` / `ATLAS_CELL` 宏切换 generator 产物，运行时类层级无须分裂。

---

## 3. 进程上下文与 RPC 角色

`DefGenerator` 看预处理符号决定 `ProcessContext`：

| 符号 | 上下文 | 含义 |
|---|---|---|
| `ATLAS_BASE` | `Base` | base_methods 收，cell/client_methods 发 |
| `ATLAS_CELL` | `Cell` | cell_methods 收，base/client_methods 发 |
| `ATLAS_CLIENT` | `Client` | client_methods 收，exposed cell/base_methods 发 |
| 无 | `Server` | 兼容回退（统一服务端项目，所有方向都收发） |

### 3.1 方法 × 上下文 → 生成行为

| 方法类型 | Base | Cell | Client (exposed) | Client (非 exposed) |
|---|---|---|---|---|
| `client_methods` | 发送 stub | 发送 stub | 用户 partial 实现 | 用户 partial 实现 |
| `cell_methods` | 发送 stub | 用户 partial 实现 | 发送 stub | Forbidden（抛 InvalidOperationException） |
| `base_methods` | 用户 partial 实现 | 发送 stub | 发送 stub | Forbidden |

### 3.2 Mailbox 生成

| Mailbox | Base | Cell | Client |
|---|---|---|---|
| `Client` / `AllClients` / `OtherClients` | ✓ | ✓ | – |
| `Cell` | ✓ | – | 仅含 exposed cell_methods |
| `Base` | – | ✓ | 仅含 exposed base_methods |

### 3.3 Dispatcher

| Dispatcher | Base | Cell | Client |
|---|---|---|---|
| `DispatchCellRpc` | – | ✓ | – |
| `DispatchBaseRpc` | ✓ | – | – |
| `DispatchClientRpc` | – | – | ✓ |

调用形式：`target.Method(args...)` —— 直接走用户的 partial 实现，无 `On` 前缀。

---

## 4. 生成产物清单

每个实体生成（按上下文裁剪）：

| 文件 | 内容 | 备注 |
|---|---|---|
| `{Class}.Properties.g.cs` | 字段、Property（含 dirty bit + `OnXxxChanged`）、`ReplicatedDirtyFlags` 枚举 | 客户端不维护 dirty bit；struct 属性走 MutRef；list/dict 走 ObservableXxx |
| `{Class}.Serialization.g.cs` | `TypeName` / `TypeId` 重写、`Serialize` / `Deserialize` 全量 | 客户端 `Deserialize` 末尾调 `OnEnterWorld()` |
| `{Class}.DeltaSync.g.cs` | 服务端：audience mask + `SerializeOwnerDelta` / `SerializeOtherDelta` / `SerializeForOwnerClient` / `SerializeForOtherClients` / `BuildAndConsumeReplicationFrame`；客户端：`ApplyOwnerSnapshot` / `ApplyOtherSnapshot` / `ApplyReplicatedDelta` | sectionMask byte（bit 0=scalars / 1=containers / 2=components）；客户端 `ApplyReplicatedDelta` 触发 `OnXxxChanged` |
| `{Class}.RpcStubs.g.cs` | 发送 stub（`SpanWriter` 序列化 + `Send{Cell,Base,Client}Rpc`）+ partial 接收声明 + Forbidden stub | exposed scope 由 RpcRoleHelper 决定 |
| `{Class}.Mailboxes.g.cs` | Mailbox 结构体 + 属性入口 | |
| `{Class}.Components.g.cs` | 槽访问器（`Combat` 属性等）+ `SyncedSlotCount` / `ResolveSyncedSlot`；服务端额外 `WriteOwnerComponentSection` / `HasOwnerDirtyComponent` / `ClearDirtyComponents`，客户端 `ApplyComponentSection` | 组件→槽映射稳定 1-based，槽 0 留给实体本体 |

全局：

| 文件 | 内容 |
|---|---|
| `EntityFactory.g.cs` | `[ModuleInitializer]` 注册 typeId → factory（服务端 `EntityFactory.Register`；客户端 `Atlas.Client.ClientEntityFactory.Register`） |
| `RpcIds.g.cs` | RPC id 常量；rpc_id 编码：方向(2 bit) + typeIndex(8 bit) + slotIdx(8 bit) + methodIdx(剩余) |
| `DefRpcDispatcher.g.cs` | switch dispatch；客户端在 `[ModuleInitializer]` 注入 `Atlas.Client.ClientCallbacks.ClientRpcDispatcher` |
| `DefStructRegistry.g.cs` | 把每个 struct 注册到 native 之前的 entity types |
| `DefEntityTypeRegistry.g.cs` | 串成 ATDF 二进制 buffer，`[ModuleInitializer]` 时刻交给 `Atlas.Runtime` / `Atlas.Client` 的 bridge 喂给 C++ `EntityDefRegistry::RegisterFromBinaryBuffer` |
| `{Component}.Component.g.cs` | 每个 synced 组件类型一份；服务端 `ReplicatedComponent` 派生，客户端 `ClientReplicatedComponent` 派生 |
| `{Struct}.Struct.g.cs` | 用户态 struct + Serialize/Deserialize；可由 `sync="whole" | "field"` 选择整块还是字段级 op-log，`Auto` 时由 `StructEmitter.DecideSyncMode` 启发式决定（DEF014 Info 透传选择） |

---

## 5. 客户端 RPC 安全校验链路

C++ 侧不解析 `.def`；`EntityDefRegistry` 的 RPC / 属性 / scope 信息都来自 generator 通过 `DefEntityTypeRegistry.g.cs` 在 `[ModuleInitializer]` 写入的 ATDF buffer。校验链分两段：

### 5.1 Client → Base（自身）

```
Client                     BaseApp
  ─ kClientBaseRpc ───────▶ OnClientBaseRpc
                            ├ 1. Channel 已认证？否则 drop
                            ├ 2. FindRpc(rpc_id) + Direction()==0x03
                            ├ 3. is_exposed 为 None？drop
                            └ 4. 路由隔离（proxy 只能到自己的 Base）
                              dispatch → C# 用户 partial
```

跨实体 base RPC 在客户端层就被 generator 阻断（不为非自身的 Base mailbox 生成发送 stub），加之架构禁止（DEF003），等价于 BigWorld 的 § 5.1 规则。

### 5.2 Client → Cell（自身或其他实体）

```
Client                     BaseApp                          CellApp
  ─ kClientCellRpc(target,rpc) ▶ OnClientCellRpc
                                ├ 1. 认证 + FindRpc + Direction()==0x02
                                ├ 2. 非 exposed → drop
                                ├ 3. target!=proxy.entity_id 且
                                │    exposed!=AllClients → drop
                                ├ 4. resolve cell channel
                                └ 5. 嵌入 source_entity_id=proxy.entity_id
                                  ─ kClientCellRpcForward ────▶ OnClientCellRpcForward
                                                              ├ 6. trusted_baseapps 集合校验
                                                              ├ 7. FindRealEntity + IsReal
                                                              ├ 8. exposed==OwnClient 且
                                                              │    source!=target → drop
                                                              └ 9. dispatch → C# 用户 partial
```

`source_entity_id` 客户端无法伪造（在 BaseApp 由可信 proxy 信道决定）。CellApp 自防御复查（步骤 6-8）即便上游被攻陷也不会越权。Ghost 在第 7 步直接拒绝（`IsReal()` 失败）—— 客户端只能调 Real 实体的 cell 方法。

### 5.3 可达性矩阵

| 路径 | 允许？ | 条件 |
|---|---|---|
| Client → 自身 Base | ✓ | exposed = own_client / true |
| Client → 其他实体 Base | ✗ | 架构禁止（无路由 + DEF003） |
| Client → 自身 Cell | ✓ | exposed = own_client / all_clients / true |
| Client → 其他实体 Cell | ✓ | exposed = all_clients（仅此值） |

---

## 6. 诊断码

| ID | 级别 | 规则 |
|---|---|---|
| ATLAS_DEF001 | Error | `[Entity("X")]` 找不到匹配 `.def` |
| ATLAS_DEF002 | Error | `client_methods` 标了 `exposed` |
| ATLAS_DEF003 | Error | `base_methods` 用了 `exposed="all_clients"` |
| ATLAS_DEF006 | Error | `.def` XML 解析失败 |
| ATLAS_DEF007 | Error | 重复 type_id |
| ATLAS_DEF008 | Warning | 属性名 `position` 与 volatile 通道撞车，已自动跳过 |
| ATLAS_DEF009 | Error | type 表达式语法错误 |
| ATLAS_DEF010 | Error | type 表达式嵌套超深 |
| ATLAS_DEF011 | Error | `dict[K,V]` 的 K 必须是标量 |
| ATLAS_DEF012 | Error | `<types>` 内 struct 重名 |
| ATLAS_DEF013 | Error | struct 引用环 |
| ATLAS_DEF014 | Info | struct 自动同步策略选择（Whole / Field）透出 |
| ATLAS_DEF015 | Error | 实体属性 / 组件槽名 / 组件 type 重名 |
| ATLAS_DEF016 | Error | struct 与 alias 名冲突 |
| ATLAS_DEF017 | Error | alias 引用链超深或成环 |
| ATLAS_DEF018 | Error | `client_methods` 声明了 `reply` |
| ATLAS_RPC001 | Warning | `RpcReply<T>.Value` 未先检查 `IsOk` 就访问（Roslyn analyzer，运行期会抛） |

---

## 7. 客户端属性变更回调语义

对齐 BigWorld `simple_client_entity.cpp::propertyEvent()` 与 `bw_entity.cpp::onPositionUpdated()`：

- `Deserialize`（初始快照 / 进入 AoI）直写字段，**不**触发 `OnXxxChanged`，避免初始状态当作"变化"误投递。完成后调 `OnEnterWorld()`，脚本在此基于完整状态做整体初始化。
- `ApplyOwnerSnapshot` / `ApplyOtherSnapshot`（基线通道 0xF002 / kEntityEnter 重新进 AoI）同上 —— snapshot 是权威重置不是观测变化。
- `ApplyReplicatedDelta`（增量通道 0xF003）每个变化字段触发 `OnXxxChanged(old, new)`，与服务端一致。
- `ApplyPositionUpdate` 走专用通道，触发 `OnPositionUpdated(newPos)`，**不**经属性回调链。Position 永不出现在 ApplyReplicatedDelta 位图中（DEF008）。

---

## 8. 验证

```bash
dotnet build src/csharp/Atlas.Generators.Def/      # 构建 generator
dotnet test  tests/csharp                          # generator + 运行时测试
cmake --build build/debug --config Debug           # C++ 构建
ctest --build-config Debug -L unit                  # C++ 单元测试
```

落到磁盘的 `.g.cs` 在 `obj/<config>/<tfm>/generated/Atlas.Generators.Def/Atlas.Generators.Def.DefGenerator/` 下；diff 这个目录是审计 generator 行为最直接的方式。
