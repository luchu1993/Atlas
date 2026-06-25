# UE 客户端架构

> 状态: 当前 UE MVP 架构。Layer 1/2/3 分层、Entity owns View、AtlasUE plugin
> 接入方式已落地；表现层资产和高级调试流程仍以 MVP README 的缺口为准。
>
## 核心翻转:Entity 拥有 View

Atlas Entity 是状态真理。UE Actor 只是它的一个可选视图。

- **Entity 在 Layer 1**(引擎无关 C++),由 net 事件创建/销毁
- **View 是 Entity 的附属物**,实现 `atlas::EntityView` 接口
- **Actor 不持有 Entity 状态**,View 通过 `TWeakObjectPtr<AActor>` 弱引用 Actor

### 为什么是这个方向

1. Entity 可以无 Actor 存在(AoI 外、对象池、刚死亡仍在播动画)
2. Actor 被 GC 或关卡卸载不影响 Entity 状态
3. 同一 Entity 可挂多个 View(3D Actor + minimap icon)
4. 多客户端引擎天然支持 —— 其它 C++ 宿主实现自己的 `EntityView` 即可

## 三层分层

| Layer | 内容 | 引擎依赖 |
|-------|------|---------|
| **1** 纯 C++ Atlas client layer | `ClientEntityManager`、`ClientEntity`、`AvatarFilter`、`EntityView` 接口、codegen 产出的 `atlas::mvp::Avatar` 等 typed wrapper、logic component 纯 C++ 类 | 无 |
| **2** `AtlasUE` Plugin | `UAtlasSubsystem`(per-`UWorld`)、`FAtlasUEActorView`、Login / HUD / SpaceData 基类、坐标/单位转换 | UE |
| **3** 游戏项目 | `AAvatarCapsule` 业务 Actor、`UAtlasAvatarView` 表现层 bridge、输入控制器、generated entity stubs | UE + 游戏 |

Layer 1 的通用实现位于 AtlasUE plugin 的 `Public/AtlasCore/` 与
`Private/AtlasCore/`;`samples/mvp/UEClient/Source/UEClient/gen/` 下的
generated stubs 也只依赖这层和标准 C++。这一层不依赖任何 UE 类型,
理论上可以被其它 C++ 主机进程复用(测试 harness、未来 Unity C++ SDK
迁移等)。

## `.def <component>` ≠ UE `UActorComponent`

完全两个概念,数量上不对应,职责不重叠。

| | `.def <component>` | UE `UActorComponent` |
|---|---|---|
| 含义 | Atlas 逻辑组件,带属性 + RPC,跟 entity 复制 | UE 表现层组件 |
| 例子 | `CombatComponent` / `InventoryComponent` / `AIComponent` | `SkeletalMesh` / `AnimBP` / `Camera` |
| 所在层 | Layer 1,纯 C++ 类 | Layer 3,游戏侧手写 |
| 谁生成 | codegen | 不生成,游戏程序员写 |
| 持有 Atlas 状态? | 是 | **否** |

Logic component 的字段和 RPC 在 `UAtlasAvatarView` 上**平铺暴露给 BP**(`GetAttackPower()` 内部访问 `Combat` 组件)。**不**为每个 logic component 生成独立 UActorComponent,避免 BP 树臃肿。

游戏侧的表现层 UActorComponent 通过 `UAtlasAvatarView` 读取所需字段,**不直接持有 Atlas 状态**。Atlas 数据流伸进 UE 只有一条入口:`UAtlasAvatarView`。

## Tick / 线程模型

- `atlas_net_client` 网络栈跑在独立 `FRunnable` 线程,poll 出的数据入跨线程队列
- GameThread tick:
  1. 消费 entity enter 队列 → 查类型注册表 → `SpawnActor` → `AttachView`
  2. 消费 entity leave 队列 → `DetachView` → Actor 自然 `Destroy`
  3. transform 通道 → `AvatarFilter` step → `SetActorLocation`
  4. 属性 delta → `EntityView::OnPropertyChanged` / typed hook → BP delegate
  5. 上行 RPC 队列 flush

`AvatarFilter` 的 EMA 时钟同步对 dt 抖动敏感。net poll 独立线程保证"网络时间"不受 GT 抖动影响。

## 不用 UE 的什么

| 系统 | 不用的原因 |
|------|----------|
| NetDriver / Replication | Atlas 自有 wire 协议负责所有状态/RPC |
| `UFUNCTION(Server/Client)` | RPC 走 codegen 出的纯 C++ stub;BP 暴露由手写 view / widget 层提供 |
| GAS | 与 Atlas 服务端权威模式冲突 |
| Iris | 不需要 |
| Mass | 不需要 |
| `CharacterMovementComponent` 网络部分 | 禁用;本地运动学保留 |

可以用的:Enhanced Input(输入采集后转 Atlas movement input frame 或低频 RPC)、
Blueprint / UMG(UI 与业务逻辑)、动画系统(以 Atlas 字段为输入驱动)。
