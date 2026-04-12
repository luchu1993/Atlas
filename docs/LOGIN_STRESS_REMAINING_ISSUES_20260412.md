# Atlas 登录链路极限压测残余问题记录

> 记录日期: 2026-04-12
> 范围: LoginApp / BaseApp / BaseAppMgr / DBApp 在极端短线重登压力下的实现现状、压测结果与残余问题

配套压测执行方法、参数说明和结果判读，见 [LOGIN_STRESS_TESTING.md](./LOGIN_STRESS_TESTING.md)

---

## 1. 背景

当前登录链路已经按 BigWorld 风格的单线程、多进程架构完成主线实现，并针对高并发短线重登场景做了多轮性能和稳定性修复。

本记录只关注一个问题域:

- 极端短线重登压力下，`force-logoff -> relogin -> checkout -> authenticate` 链路是否还能保持稳定、低延迟和可恢复

---

## 2. 本轮已落地改动

### 2.1 主线改动

1. **BaseApp 本地重登快路径与 detached grace**
   - 为重复登录链路补齐本地旧 Proxy 脱离、短暂保留和重登衔接逻辑，减少旧连接刚断开时立即重建带来的状态抖动。
   - 优化 `force-logoff` 之后的重试状态推进，避免未满足条件时过早重新 checkout。

2. **BaseAppMgr 负载计算与上报增强**
   - 区分测量负载和分配负载，改善 `InformLoad` 语义，使 BaseAppMgr 的选择更接近真实承载能力。
   - 为后续按真实瓶颈做负载均衡打基础，避免仅按实体数或瞬时值做粗糙分配。

3. **DB/XML 写回合并与分阶段落盘**
   - 对 XML 数据库写回做 staged/coalesced 处理，降低高频 logout / writeToDB 时的重复写入开销。
   - 缓解短线重登时 `logoff -> writeback -> recheckout` 对 DB 层的反复冲击。

### 2.2 补充修复

1. **内部服务间 RUDP 改为 `connect_rudp_nocwnd()`**
   - 已切换 `LoginApp`、`BaseApp`、`DBApp` 的内部 server-to-server 链路，降低极端并发下拥塞窗口对本机/局域网消息流的额外限制。

2. **ReliableUdp condemned 状态彻底清理**
   - 通道进入 condemn 后，主动清理 resend、ack 和 reassembly 状态，避免残留状态跨重连/复用路径干扰后续消息处理。

3. **force-logoff 重试状态机收紧**
   - 避免收到 `ForceLogoffAck` 或进入重试窗口后过早推进到重新 checkout，减少链路自激和无效重试。

4. **补充单元测试**
   - 新增 `tests/unit/test_reliable_udp.cpp`，覆盖 ReliableUdp 生命周期和清理行为。

---

## 3. 已验证结果

### 3.1 定向测试

以下测试已通过:

```powershell
ctest --test-dir build\debug-windows -C Debug --output-on-failure -R "(ReliableUdp|XmlDatabase|BaseAppMgrIntegration|BaseAppMgrMessages|CheckoutManager|EntityManager)"
```

### 3.2 压测结果

1. **单 BaseApp 高压场景**
   - 产物目录: `.tmp/login-stress/20260412-145327`
   - 结果: `timeout_fail = 800`
   - 结论: 相比此前约 `4000+` 级别的超时，已有明显改善，但距离“极限稳定”仍然不够。

2. **4 BaseApp / 3200 客户端极限场景**
   - 产物目录: `.tmp/login-stress/20260412-145720`
   - 结果: 每个 worker 仍出现约 `3900` 次 `timeout_fail`
   - 同时观察到:
     - `invalid_session`
     - `no_dbapp`

### 3.3 关键观测

在 `.tmp/login-stress/20260412-145327/db/checkouts.json` 中，可以看到压测结束时仍有 800 个账号保持为 BaseApp checkout 状态。

这说明残余瓶颈已经不只是“写回太慢”或“DB 没释放”，而更可能是:

- BaseApp 本地仍长期持有旧 Proxy / checkout 所有权
- `force-logoff -> detach -> writeback -> checkin -> recheckout` 链路某个阶段没有及时收敛
- 多 BaseApp 并发下，登录回复或 DB 协调消息出现了排队、饥饿或延后

---

## 4. 当前仍未解决的问题

### 4.1 极端短线重登下，`force-logoff -> relogin` 链路仍会被打满

现象:

- 单 BaseApp 场景已经显著改善，但在极限短线重登下仍有大量超时。
- 多 BaseApp 并发时问题进一步放大，说明仅靠本地重试优化还不够。

影响:

- 登录成功率在极端 churn 下仍不足以支撑高强度回归压测。
- BaseApp 进程内的登录生命周期状态还存在积压或无法及时收敛的问题。

### 4.2 `invalid_session` 说明登录成功通知与客户端认证链路存在错位

现象:

- 压测中出现 `LoginOk` 已返回，但客户端到 BaseApp `Authenticate(session_key)` 时失败。

可能原因:

- Session 生命周期过早清理
- 旧登录/新登录状态重叠时，session key 对应的 pending proxy 被替换或回收
- 极端排队下，客户端拿到的是已失效或已被后续流程覆盖的 session

### 4.3 `no_dbapp` 说明 DB 路径在突发多进程场景下仍可能成为拒绝点

现象:

- 4 BaseApp 极限场景下出现 `no_dbapp`

这类错误在当前阶段不应简单理解为“DBApp 挂了”，更可能是:

- DBApp 忙于处理 checkout / checkin / writeback，导致登录路径超时后被上层当作不可用
- BaseApp / LoginApp 在等待窗口内没有拿到有效响应，触发兜底失败
- 多进程压力下，DB 回复链路存在局部饥饿

### 4.4 checkout 长时间不释放，说明本地所有权状态存在滞留

从 `checkouts.json` 的结果看，大量账号在压测末尾仍被 BaseApp 持有。

这说明需要继续核查 BaseApp 内部以下状态是否存在滞留或顺序竞争:

- `active_login_dbids_`
- `logoff_entities_in_flight_`
- `pending_logoff_writes_`
- `pending_force_logoffs_`

---

## 5. 初步结论

当前主线 1/2/3 的方向是正确的，且已经明显改善了单进程场景下的极端重登表现，但系统距离“多 BaseApp 极限压力下可稳定恢复”还有明显差距。

更准确地说，当前问题已经从“显而易见的 O(n) 热路径和粗糙生命周期管理”阶段，进入了“跨进程状态收敛与排队稳定性”阶段。接下来需要用更细粒度的状态观测和时延分解来定位链路到底卡在:

- BaseApp 本地旧 Proxy 释放
- DBApp checkout / checkin 协调
- LoginApp 到 BaseApp 的 PrepareLogin 完成回包
- 客户端拿到 SessionKey 后的最终认领

---

## 6. 下一步高优先级排查方向

### 6.1 给 BaseApp 登录生命周期补充更强的可观测性

需要对以下阶段增加精确计数和时延采样:

1. `PrepareLogin` 进入
2. 首次 checkout 失败
3. `ForceLogoff` 发出
4. `ForceLogoffAck` 收到
5. 旧 Proxy detach 完成
6. writeback / checkin 开始与完成
7. retry checkout 发起与成功
8. `PrepareLoginResult` 返回
9. `Authenticate(session_key)` 成功或失败

目标:

- 把“超时”拆解成具体卡点，而不是继续靠总超时计数猜测瓶颈。

### 6.2 核查 BaseApp 本地状态机是否存在未清理或重入推进

重点检查:

- 旧 Proxy 已经 detached，但登录上下文仍未解除占用
- 同一 `dbid` 上存在多份并发中的重登上下文
- `ForceLogoffAck`、writeback 完成、checkout 重试之间的推进顺序仍然存在竞争窗口

### 6.3 核查 DBApp 在多 BaseApp 突发下的排队与优先级

重点检查:

- checkout / checkin / writeback 是否共享同一条拥塞队列
- 登录关键路径是否会被批量写回淹没
- DB 返回消息是否存在长尾延迟，导致上层误判为失败

### 6.4 继续完善 BaseAppMgr 负载均衡输入

当前负载分配逻辑虽然已经改善，但若要真正发挥多进程极限性能，还需要继续保证:

- 分配依据体现“可接新登录能力”，而不是只体现“当前实体规模”
- 旧登录释放中的 BaseApp 会被及时降权
- 极端 churn 下不会把新登录继续灌入已经处于回收堆积状态的进程

---

## 7. 当前结论对应的代码与产物参考

### 7.1 关键代码

- `src/server/baseapp/baseapp.cpp`
- `src/server/baseapp/baseapp.hpp`
- `src/server/baseappmgr/baseappmgr.cpp`
- `src/lib/db_xml/xml_database.cpp`
- `src/server/loginapp/loginapp.cpp`
- `src/server/dbapp/dbapp.cpp`
- `src/lib/network/reliable_udp.cpp`
- `src/lib/network/reliable_udp.hpp`

### 7.2 压测产物

- `.tmp/login-stress/20260412-145327`
- `.tmp/login-stress/20260412-145720`

---

## 8. 状态判断

截至 2026-04-12:

- 常规登录链路和定向测试已具备继续推进条件
- 单 BaseApp 极端短线重登能力已明显改善
- 多 BaseApp 极限短线重登场景仍未达到可接受标准
- 当前最高优先级不再是继续堆“重试次数”或“放宽超时”，而是把跨进程生命周期收敛问题彻底量化并定位
