# 待决策 / 已延期

## 已延期(TODO)

### D2 — 共享 C++ 移动仿真库

当前 UE 客户端**纯插值不预测**(`AvatarFilter` 二阶收敛)。"超越 BDO 手感"目标需要客户端预测,而预测需要客户端与服务器跑同一份移动逻辑(避免漂移)。

按项目"C++ 移动仿真共享"决策,应有一个 `src/lib/movement_sim/` 静态库:
- 服务端 CellApp 链接
- Unity 客户端通过 P/Invoke 调用
- UE 客户端直接链接

**何时立项**:UE 端到端贯通(M3 完成)+ 实测插值手感不达标后。否则可能优化错方向。

### 战斗代码共享(诉求 C)

诉求 C(三端零漂移)有两条路:
- 路径 A:UE 寄宿 CoreCLR,跑共享 C# 战斗逻辑
- 路径 B+下沉:战斗数值核 C++ 共享库,服务端 C# 调,UE/Unity 链接

本期 UE 接入按**不做战斗预测**实现,M3 完成后视实测决策。

## 待决(M1 之前需拍板)

### CppEmitter — 属性 BP 暴露粒度

- 选项 1:全字段 UPROPERTY 缓存 — 冗余拷贝,BP 易用
- 选项 2:hot/cold 区分,`.def` 加 `@bp_cache` — 精细但要扩 `.def` 语法
- 选项 3:全 getter+registry 查表 — 简洁,BP 频繁调用有 ns 级开销

**默认选项 3**,M1 验证 BP 频繁访问的性能。若不达标,M2 之前扩 `@bp_cache`。

### Logic component 的"独立 BP 视图"需求

主架构是 logic component 平铺到 `UAtlasAvatarView`。但 `InventoryComponent` 这类可能有独立 UI BP 直接绑。

**当前方案**:游戏侧手写 `UInventoryUIData : UObject`,由 view 在 OnInventoryChanged 时刷新。**不**自动为 logic component 生成 UActorComponent。M2 验证这个 pattern 可用。

## 待决(M3 之后)

### 无缝大世界 UWorld 转移时 entity 处理

PIE 单 world OK(per-world subsystem 隔离)。但发布的无缝大世界:
- 玩家跨 Streaming Level 时 entity 仍是 server 那一份
- UE 端 Actor 可能因 streaming 卸载/重建
- View 自动 detach 后,新 level 加载需重新 SpawnActor + AttachView

需要 subsystem 在 streaming 事件上重建视图,view bridge 的弱引用机制要扛过 Actor 多次生灭。

**何时处理**:接入 World Partition 后立项。

### Hot Reload / PIE 多客户端调试

每个 PIE 实例独立 `atlas_net_client` 句柄,需要给 native DLL 加 instance ID。**何时处理**:M3 后根据开发体验需要立项。

### RPC Reply Latent UFUNCTION 暴露

wire 协议已支持 `trace_id + reply_bit`。BP latent(request-response 同步等待)是体验加分项,非必需。**何时处理**:有 BP-driven async RPC 需求时。
