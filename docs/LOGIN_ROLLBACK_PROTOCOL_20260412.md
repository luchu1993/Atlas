# Atlas 登录中途断开回滚协议设计

> 设计日期: 2026-04-12
> 适用范围: `LoginApp` / `BaseApp` / `DBApp`
> 问题域: 客户端在登录中途断开后，如何可靠回滚 `prepare/checkout`

---

## 1. 目标

本设计只解决一个明确问题:

- 客户端在登录过程中放弃本次登录后，服务端必须最终回收本次登录关联的临时状态

这里的“临时状态”包括:

- `LoginApp` 的 pending login
- `BaseApp` 的 queued / pending prepare 状态
- `DBApp` 的 in-flight checkout
- `BaseApp` 已创建但尚未 Authenticate 的 prepared proxy

本设计不试图一次性解决所有重复登录问题，尤其不把已经进入破坏性 `force-logoff` 的旧会话恢复问题混进本次协议。

---

## 2. 非目标

以下内容明确不在本次协议范围内:

- 不要求跨进程“精确一次”投递
- 不要求取消请求一定先于原始回包到达
- 不要求取消后恢复已经进入 destroy 阶段的旧会话
- 不引入重型分布式事务协调器

本次协议追求的是:

- 最终一致回收
- 全链路幂等
- 单节点职责清晰

---

## 3. 核心不变量

协议落地后，必须始终满足以下不变量:

1. 一个登录请求一旦被标记为取消，就不能再对外产生成功登录结果。
2. 一个登录请求一旦被标记为取消，迟到的 `AuthLoginResult`、`AllocateBaseAppResult`、`CheckoutEntityAck`、`PrepareLoginResult` 都只能被吞掉或转为清理动作。
3. `DBApp` 中不能因为取消和迟到回包交叉而遗留 checkout 标记。
4. `BaseApp` 中不能因为取消消息丢失而无限期保留 prepared proxy。
5. 所有取消相关消息都必须可以重复处理。

---

## 4. 角色与职责

### 4.1 LoginApp

`LoginApp` 只负责声明“这次登录已经被客户端放弃”。

它负责:

- 识别客户端断开
- 识别登录超时
- 识别 `PrepareLoginResult` 成功后但 `LoginResult` 无法送达客户端
- 将这些场景统一折叠为 cancel 动作
- 对迟到的上游结果做本地消重和吞包

它不负责:

- 直接销毁 `BaseApp` 上的 prepared entity
- 直接清理 `DBApp` checkout

### 4.2 BaseApp

`BaseApp` 是 prepare 生命周期的真正拥有者。

它负责:

- 取消 queued / pending prepare
- 回滚 in-flight checkout
- 回滚已经 prepared 但尚未 Authenticate 的代理
- 吞掉取消后的迟到 `CheckoutEntityAck`
- 为 prepared proxy 提供租约超时回收

### 4.3 DBApp

`DBApp` 只负责 checkout 生命周期。

它负责:

- 记录 in-flight checkout request
- 接收 `AbortCheckout`
- 如果 checkout 尚未完成，则标记 canceled 并在回调落地时清理
- 如果 checkout 已完成，则立即 clear checkout
- 返回 `AbortCheckoutAck`

---

## 5. 状态模型

单个登录请求在本设计里可抽象为以下逻辑状态:

| 状态 | 拥有者 | 含义 | 取消动作 |
|------|--------|------|----------|
| `AuthPending` | `LoginApp` | 正在认证或等待分配 BaseApp | 删除本地 pending |
| `PrepareQueued` | `BaseApp` | 已进入 BaseApp，但尚未发起 checkout | 从 queued / pending-force-logoff 中移除 |
| `CheckoutPending` | `BaseApp + DBApp` | `BaseApp -> DBApp` checkout 已发出 | 发送 `AbortCheckout`，记录 canceled marker |
| `Prepared` | `BaseApp` | Proxy 已创建，尚未 Authenticate | destroy proxy + release checkout |
| `Authenticated` | `BaseApp` | 客户端已绑定成功 | 退出本协议，转入正常在线生命周期 |

这个状态机的关键点不是“只前进不回退”，而是:

- `Prepared` 可以被显式 cancel 回退到“无状态”
- `CheckoutPending` 可以被显式 abort 回退到“无状态”

---

## 6. 消息协议

### 6.1 `login::CancelPrepareLogin`

方向:

- `LoginApp -> BaseApp`

语义:

- 客户端已经放弃 `request_id`
- 如果 `BaseApp` 仍在推进这条 prepare 链路，必须开始回滚

字段:

- `request_id`
- `dbid`

约束:

- 允许重复发送
- `BaseApp` 必须按当前状态幂等处理

### 6.2 `dbapp::AbortCheckout`

方向:

- `BaseApp -> DBApp`

语义:

- 某个 checkout request 已失效
- `DBApp` 必须清理该 request 对应的 checkout 状态

字段:

- `request_id`
- `type_id`
- `dbid`

### 6.3 `dbapp::AbortCheckoutAck`

方向:

- `DBApp -> BaseApp`

语义:

- `AbortCheckout` 已处理

约束:

- 只是确认，不改变最终语义
- 就算 ack 丢失，`BaseApp` 也必须靠 retention 和迟到回包吞包机制保持安全

---

## 7. 统一取消入口

`LoginApp` 内部所有“客户端已经不再需要这次登录”的场景，统一收敛到同一入口:

- 客户端主动断开
- 登录超时
- `PrepareLoginResult` 成功后，客户端通道已不存在
- `PrepareLoginResult` 成功后，`LoginResult` 发送失败

统一动作:

1. 从 `pending_` 中移除请求
2. 从 `pending_by_username_` 中移除占位
3. 将 `request_id` 记入 `canceled_requests_`
4. 如果已知目标 `BaseApp`，发送 `CancelPrepareLogin`
5. 吞掉后续迟到结果

这样可以避免“断开一套逻辑、超时一套逻辑、发送失败再一套逻辑”的状态漂移。

---

## 8. BaseApp 回滚策略

`BaseApp` 收到 `CancelPrepareLogin` 后，按以下顺序匹配状态:

1. 如果请求仍在 `pending_logins_`
   - 说明 checkout 已发出或即将处理
   - 记录 `canceled_login_checkouts_`
   - 发送 `AbortCheckout`
   - 结束该 dbid 对应的 login flow

2. 如果请求仍在 `pending_force_logoffs_`
   - 说明 prepare 还卡在重复登录处理阶段
   - 直接移除 pending
   - 结束该 dbid 的 login flow

3. 如果请求仍在 `queued_logins_`
   - 说明尚未真正执行 checkout
   - 直接删除 queued 项

4. 如果请求落在 `deferred_login_checkouts_`
   - 说明在等待冲突实体完成处理
   - 释放已持有 checkout
   - 删除 deferred 项

5. 如果请求已经进入 `prepared_login_entities_`
   - 说明 proxy 已创建但客户端还没 Authenticate
   - destroy proxy
   - clear session
   - unbind client
   - release checkout

6. 如果以上都不命中
   - 视为迟到 cancel
   - 只记录 debug，不再报错

---

## 9. 迟到回包处理

### 9.1 LoginApp

`LoginApp` 对以下回包都做 canceled 检查:

- `AuthLoginResult`
- `AllocateBaseAppResult`
- `CheckoutEntityAck`
- `PrepareLoginResult`

如果 request 已在 `canceled_requests_` 中:

- 直接忽略
- 不做错误日志升级

### 9.2 BaseApp

`BaseApp` 对迟到 `CheckoutEntityAck` 的处理规则:

- 如果 request 在 `canceled_login_checkouts_` 中
  - 若 ack 为 success，则立即 `release_checkout`
  - 删除 canceled marker
  - 不再继续 prepare

这保证了 cancel 与 checkout 成功回包交叉到达时，最终不会留下脏 checkout。

### 9.3 DBApp

`DBApp` 的 in-flight checkout 在回调闭包中重新检查:

- request 是否还在 `pending_checkout_requests_`
- 是否已被标记 canceled

如果已 canceled:

- `release_checkout`
- `mark_checkout_cleared`
- 不发送原始 `CheckoutEntityAck`

---

## 10. 租约与超时

### 10.1 Cancel retention

目的:

- 吞掉迟到消息

当前窗口:

- `LoginApp::kCanceledRequestRetention = 10s`
- `BaseApp::kCanceledCheckoutRetention = 10s`

### 10.2 Prepared lease

目的:

- 防止 `CancelPrepareLogin` 丢失时 `Prepared` 状态永久悬挂

当前窗口:

- `BaseApp::kPreparedLoginTimeout = 10s`

语义:

- proxy 已 prepare 完成，但客户端在租约期内未 Authenticate
- `BaseApp` 自动执行 prepared rollback

这层租约是本次协议最重要的兜底，不依赖 cancel 消息可靠送达。

---

## 11. 故障模型

本协议默认以下故障都可能发生:

- cancel 消息丢失
- ack 消息丢失
- 原始成功回包晚于 cancel 到达
- 同一 cancel 重复投递
- `LoginApp` 在准备发客户端成功结果前失去客户端通道

本协议不依赖“不会丢”“不会重排”这种前提，而是靠下面三件事收敛:

- 幂等状态机
- canceled retention
- prepared lease timeout

---

## 12. 当前已覆盖的行为

当前实现已经覆盖:

- 客户端断开后通知 `BaseApp`
- `BaseApp` 通知 `DBApp` abort checkout
- `DBApp` 对 pending checkout 取消并 suppress 原始回包
- `BaseApp` 对迟到 checkout success 做清理而非继续推进
- `Prepared` 后客户端不再出现时的自动回收
- `LoginApp` 超时和发送失败统一走 cancel 路径

---

## 13. 测试矩阵

建议把测试分成三层:

### 13.1 消息层

- `CancelPrepareLogin` round-trip
- `AbortCheckout` round-trip
- `AbortCheckoutAck` round-trip

### 13.2 组件行为层

- `LoginApp` 断开后对应 request 进入 canceled 集
- `BaseApp` 收到 cancel 后正确命中 queued / pending / prepared 分支
- `DBApp` 收到 abort 后 pending request 被 canceled 并 suppress ack

### 13.3 集成时序层

- `CheckoutEntityAck` 晚于 cancel 到达
- `PrepareLoginResult` 成功后客户端已消失
- prepared proxy 超过租约未 Authenticate
- cancel 重发
- `AbortCheckoutAck` 丢失

当前仓库已经具备消息层验证，还建议继续补组件行为层和集成时序层。

### 13.4 推荐优先级

建议按下面顺序补，先拿到最高价值的收敛证明:

1. `CheckoutPending` 取消后，`DBApp` 不遗留 checkout
2. `Prepared` 超时后，`BaseApp` 能自动 destroy proxy 并 release checkout
3. `PrepareLoginResult` 成功但 `LoginResult` 未送达时，最终仍走 cancel rollback
4. cancel 与 `CheckoutEntityAck` 交叉到达时，最终无脏状态
5. cancel 重发和 ack 丢失场景下，最终仍能收敛

---

## 14. 可执行验收清单

建议把这组清单作为本协议上线前的最低验收门槛。

### 14.1 已完成

- [x] 消息定义已落库:
  - `login::CancelPrepareLogin`
  - `dbapp::AbortCheckout`
  - `dbapp::AbortCheckoutAck`
- [x] `LoginApp` 已将断开、超时、成功回包无法送达统一收敛为 cancel
- [x] `BaseApp` 已支持 queued / pending / prepared 三类状态的回滚
- [x] `DBApp` 已支持 pending checkout abort 与迟到回包 suppress
- [x] 消息 round-trip 测试已补齐
- [x] rollback 相关 watcher / metric 已落库
- [x] 已补最小行为测试，覆盖 `LoginApp` abandon、`BaseApp` prepared lease timeout、`DBApp` pending abort 幂等

### 14.2 待补行为测试

- [x] `LoginApp` 客户端放弃登录后，目标 request 进入 canceled 集并更新 rollback metric
- [ ] `LoginApp` 登录超时后，若已分配 `BaseApp`，会实际发送 `CancelPrepareLogin`
- [ ] `BaseApp` 收到 `CancelPrepareLogin` 且 request 位于 `pending_logins_` 时，会发 `AbortCheckout`
- [x] `BaseApp` prepared lease timeout 到期时，会回滚 prepared 状态并更新 metric
- [ ] `BaseApp` 收到 `CancelPrepareLogin` 且 request 位于 `prepared_login_entities_` 时，会 destroy proxy 并 clear session
- [ ] `DBApp` 收到 `AbortCheckout` 且 checkout 仍在 pending 时，不发送原始 `CheckoutEntityAck`
- [x] `DBApp` 收到重复 `AbortCheckout` 时保持幂等

### 14.3 待补时序测试

- [ ] `CancelPrepareLogin` 先到，`CheckoutEntityAck(success)` 后到，最终 checkout 被清理
- [ ] `CheckoutEntityAck(success)` 先到，cancel 后到，最终 prepared proxy 被回滚
- [ ] `PrepareLoginResult(success)` 已生成，但客户端通道已消失，最终 prepared proxy 被回滚
- [ ] `CancelPrepareLogin` 丢失，仅依赖 prepared lease timeout，最终仍能回收
- [ ] `AbortCheckoutAck` 丢失，`BaseApp` 仍不会因为迟到回包而泄漏状态

### 14.4 建议监控项

上线前建议至少暴露以下 watcher / metric:

- [x] `loginapp/canceled_request_count`
- [x] `loginapp/abandoned_login_total`
- [x] `baseapp/canceled_checkout_count`
- [x] `baseapp/prepared_login_timeout_total`
- [x] `dbapp/abort_checkout_total`
- [x] `dbapp/abort_checkout_pending_hit_total`
- [x] `dbapp/abort_checkout_late_hit_total`

### 14.5 建议验收命令

当前消息层与网络层建议至少固定执行:

```powershell
cmake --build --preset debug-windows --target `
  atlas_dbapp_lib atlas_baseapp_lib atlas_loginapp_lib `
  test_dbapp_messages test_login_messages test_network_interface

build\debug-windows\bin\tests\Debug\test_dbapp_messages.exe
build\debug-windows\bin\tests\Debug\test_login_messages.exe
build\debug-windows\bin\tests\Debug\test_network_interface.exe
```

---

## 15. 已知限制

当前仍有一个刻意保留的限制:

- 如果重复登录已经把旧会话推进到破坏性 `force-logoff` 阶段，取消新登录并不能恢复旧会话

原因不是实现偷懒，而是当前 `force-logoff` 语义本身就是 destructive:

- 一旦进入 `finalize_force_logoff()`
- 旧实体已经被 destroy
- 这时取消新登录只能停止后续 prepare，不能“反向复活”旧实体

---

## 16. 下一阶段建议

如果目标是更严格的工业级 MMO 语义，下一阶段应把 `force-logoff` 本身重构成可取消 saga，而不是立即 destroy:

1. 旧会话先进入 `ReloginReplacing`
2. 冻结客户端输入与迁移
3. 新登录进入 `PrepareCommitted` 前，不真正销毁旧会话
4. 新登录取消时，可恢复旧会话
5. 新登录完成 commit 后，再原子结束旧会话

这是一项单独工程，不建议和本次 rollback protocol 混做一个补丁。

---

## 17. 落地建议

推荐落地顺序:

1. 保持当前 rollback protocol 作为主线实现
2. 补充组件行为测试和时序测试
3. 为取消、prepared lease、abort checkout 增加监控指标
4. 单独排期可取消 `force-logoff saga`

这样推进，风险可控，也符合 Atlas 当前的单线程多进程架构。
