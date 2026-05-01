# Atlas 登录链路压测说明

> 更新时间: 2026-04-12
> 适用范围: LoginApp / BaseApp / BaseAppMgr / DBApp 的端到端登录压测与极端短线重登压测

---

## 1. 目标

本说明用于统一 Atlas 当前登录链路压测的执行方式、结果产物位置和问题判读口径。

当前压测重点分为两类:

1. **常规登录压测**
   - 验证登录成功率、认证时延和基础稳定性。

2. **极端短线重登压测**
   - 验证 `force-logoff -> relogin -> checkout -> authenticate` 链路在高 churn 下的收敛能力。

---

## 2. 当前工具

### 2.1 集群拉起脚本

- `tools/cluster_control/run_login_stress.py`

作用:

- 自动拉起本地 `machined`、`loginapp`、`baseapp`、`dbapp`、`baseappmgr`
- 自动生成压测运行目录
- 自动启动一个或多个 `login_stress` worker
- 结束后默认自动停服

### 2.2 压测客户端

- `src/tools/login_stress/main.cpp`

作用:

- 直接对 LoginApp 发起端到端登录
- 在登录成功后继续完成 BaseApp 认证
- 支持 account pool 小于 client 数量，制造重复登录 / 重登压力
- 支持按比例触发短线断开重连

---

## 3. 环境前提

执行压测前应确保:

1. 已完成目标配置的构建
   - 默认脚本使用 `build/debug-windows`

2. 以下程序存在于构建输出目录:
   - `machined`
   - `atlas_loginapp`
   - `atlas_baseapp`
   - `atlas_dbapp`
   - `atlas_baseappmgr`
   - `atlas_tool`
   - `login_stress`

3. C# Runtime 存在:
   - `runtime/atlas_server.runtimeconfig.json`
   - `build/.../Atlas.Runtime.dll`

4. 当前机器允许本地占用以下端口段:
   - `20013` LoginApp 外部端口
   - `20018` machined
   - `21001+` BaseApp internal
   - `22001+` BaseApp external
   - `23001` BaseAppMgr
   - `24001` DBApp

---

## 4. 推荐执行方式

### 4.1 常规单机登录压测

适用:

- 功能回归后先确认登录链路没有明显退化
- 先看成功率和基本延迟

```powershell
tools\bin\run_login_stress.bat `
  --build-dir build/debug-windows `
  --config Debug `
  --clients 200 `
  --account-pool 200 `
  --duration-sec 120 `
  --ramp-per-sec 100 `
  --baseapp-count 1 `
  --shortline-pct 0
```

判读重点:

- `login_fail`
- `auth_fail`
- `timeout_fail`
- `auth_latency_p95`
- `auth_latency_p99`

### 4.2 单 BaseApp 短线重登压测

适用:

- 验证本地 fast-relogin / detached grace / force-logoff 链路
- 观察单进程内状态机是否能收敛

```powershell
tools\bin\run_login_stress.bat `
  --build-dir build/debug-windows `
  --config Debug `
  --clients 800 `
  --account-pool 200 `
  --duration-sec 300 `
  --ramp-per-sec 300 `
  --baseapp-count 1 `
  --shortline-pct 80 `
  --shortline-min-ms 1000 `
  --shortline-max-ms 3000 `
  --hold-min-ms 3000 `
  --hold-max-ms 8000
```

判读重点:

- `timeout_fail` 是否持续上升
- `invalid_session`
- SQLite `entities.checked_out` 是否在结束后仍残留大量 checkout

### 4.3 多 BaseApp 极限短线重登压测

适用:

- 验证 BaseAppMgr 负载均衡、DB 路径协调和多进程重登链路
- 当前是最高优先级问题场景

```powershell
tools\bin\run_login_stress.bat `
  --build-dir build/debug-windows `
  --config Debug `
  --clients 3200 `
  --account-pool 800 `
  --duration-sec 300 `
  --ramp-per-sec 1200 `
  --baseapp-count 4 `
  --local-workers 4 `
  --shortline-pct 80 `
  --shortline-min-ms 1000 `
  --shortline-max-ms 3000 `
  --hold-min-ms 3000 `
  --hold-max-ms 8000 `
  --verbose-failures
```

判读重点:

- 每个 worker 的 `timeout_fail`
- `invalid_session`
- `no_dbapp`
- 各 BaseApp 是否出现明显失衡

### 4.4 多源 IP / 多 worker 压测

适用:

- 绕开 per-IP 限速的本地压测
- 验证 trusted CIDR 与 worker 拆分配置

可选参数:

- `--source-ip <ipv4>` 可重复
- `--source-ip-file <path>`
- `--local-workers <n>`
- `--worker-index <n>`
- `--worker-count <n>`
- `--login-rate-limit-trusted-cidr <cidr>`

说明:

- `run_login_stress.py` 会把 `source-ip` 按 worker 自动切分
- 如果本地压测受 LoginApp 限速影响，优先检查:
  - 是否配置了足够的 source IP
  - 是否需要把压测源网段加入 `trusted_cidr`

---

## 5. 关键参数说明

### 5.1 并发与账号重叠

- `--clients`
  - 虚拟客户端总数

- `--account-pool`
  - 可用账号池数量
  - 小于 `clients` 时，会明显增加重复登录和重登冲突

- `--account-index-base`
  - worker 分片时的账号偏移起点

经验:

- `clients == account-pool`
  - 更接近常规登录压测

- `clients >> account-pool`
  - 更接近极端短线重登压测

### 5.2 时间模型

- `--duration-sec`
  - 总压测时长

- `--ramp-per-sec`
  - 每秒发起的新登录尝试数

- `--retry-delay-ms`
  - 登录失败、认证失败或断线后的重试延时

- `--connect-timeout-ms`
  - 登录/认证阶段超时

### 5.3 在线保持与短线

- `--hold-min-ms` / `--hold-max-ms`
  - 正常在线会话保留时长

- `--shortline-pct`
  - 登录成功后会在短时间内主动断线的比例

- `--shortline-min-ms` / `--shortline-max-ms`
  - 短线触发时间窗口

经验:

- `shortline-pct = 0`
  - 适合常规登录吞吐和稳定性回归

- `shortline-pct >= 50`
  - 适合验证重复登录/强踢/重登链路

---

## 6. 运行产物

每次运行会在以下目录生成产物:

- `.tmp/login-stress/<timestamp>/`

目录结构:

- `logs/`
  - 各服务和各 worker 的 stdout / stderr

- `db/`
  - SQLite 数据目录
  - 默认生成 `atlas_login_stress.sqlite3`
  - 可用于检查压测结束后的账号/checkout 落库状态

- `dbapp.json`
  - 本次压测自动生成的 DBApp 运行配置

典型文件:

- `.tmp/login-stress/<timestamp>/logs/loginapp.stdout.log`
- `.tmp/login-stress/<timestamp>/logs/baseapp.stderr.log`
- `.tmp/login-stress/<timestamp>/logs/login_stress_worker_00.stdout.log`
- `.tmp/login-stress/<timestamp>/db/atlas_login_stress.sqlite3`

---

## 7. 结果判读

### 7.1 login_stress 实时输出

`login_stress` 会按秒打印概要，例如:

```text
[  30s] started=... login_ok=... auth_ok=... login_fail=... auth_fail=... timeouts=... online=... inflight=...
```

重点字段:

- `login_ok`
  - LoginApp 返回成功的次数

- `auth_ok`
  - BaseApp `Authenticate` 成功的次数

- `login_fail`
  - LoginApp 层失败

- `auth_fail`
  - BaseApp 认证阶段失败

- `timeouts`
  - 登录或认证阶段超时

- `online`
  - 当前在线会话数

- `inflight`
  - 当前仍在进行中的会话数

### 7.2 最终 Summary

重点关注:

- `timeout_fail`
- `planned_disconnect`
- `unexpected_disc`
- `auth_latency_p50`
- `auth_latency_p95`
- `auth_latency_p99`

判读建议:

1. `login_success` 高但 `auth_success` 明显低
   - 优先看 `invalid_session`
   - 说明 LoginApp 成功返回与 BaseApp 认证阶段之间存在错位

2. `timeout_fail` 高
   - 优先看 BaseApp / DBApp / LoginApp 日志时间线
   - 再看 SQLite `entities.checked_out` 是否有大量残留

3. `unexpected_disc` 高
   - 优先看网络层问题、进程退出、RUDP 通道状态和服务端主动断开

### 7.3 DB checkout 残留判读

文件:

- `.tmp/login-stress/<timestamp>/db/atlas_login_stress.sqlite3`

如果压测结束后 SQLite 中仍有大量 `entities.checked_out = 1`，通常说明:

- 旧 Proxy / entity 所有权未释放
- `writeback -> checkin -> recheckout` 链路没有收敛
- 本地登录状态机存在滞留

这类问题在极端短线重登场景下比单纯的登录失败数更关键。

---

## 8. 常见失败模式

### 8.1 `invalid_session`

含义:

- LoginApp 已返回成功，但 BaseApp `Authenticate(session_key)` 失败

优先排查:

- Session 生命周期是否过早清理
- 本地 fast-relogin 是否覆盖了旧/新 session 绑定
- pending proxy 是否在客户端认领前被销毁或替换

### 8.2 `no_dbapp`

含义:

- 登录准备阶段无法继续走 DB 路径

优先排查:

- DBApp 是否真实退出
- DBApp 是否在极端压力下回复饥饿
- BaseApp 是否在等待窗口内把慢回复误判为不可用

### 8.3 `timeout_fail`

含义:

- 登录或认证阶段在配置时间内未完成

优先排查:

- BaseApp 本地重登状态机
- DBApp checkout / checkin / writeback 队列
- BaseAppMgr 分配是否把流量持续打到已拥塞实例

### 8.4 `unexpected_disc`

含义:

- 非计划断开

优先排查:

- 服务端进程是否异常退出
- RUDP 通道是否被 condemn
- 登录成功后是否被旧会话/新会话互相踢掉

---

## 9. 当前已知高压样本

截至 2026-04-12，可参考以下产物目录:

- `.tmp/login-stress/20260412-145327`
- `.tmp/login-stress/20260412-145720`

---

## 10. 推荐流程

每次登录链路相关改动后，建议固定按以下顺序回归:

1. 跑定向单元测试和集成测试
2. 跑 `shortline-pct=0` 的常规登录压测
3. 跑单 BaseApp 的短线重登压测
4. 跑多 BaseApp 的极限短线重登压测
5. 检查 `logs/` 与 `db/atlas_login_stress.sqlite3`
6. 再更新专项问题记录文档

这样可以避免一上来只看极端压测，把基础功能回归和多进程极限问题混在一起。
