# Phase 13 Follow-up — 共享 snapshot 存储 / WAL

**Status:** 📐 设计 RFC(未实现)
**Owner:** Phase 13 高可用
**Prereq:** Phase 13 已落地的 CellAppMgr / BaseAppMgr snapshot
(`src/lib/server/snapshot_envelope.h`)

## 1. 问题

CellAppMgr 和 BaseAppMgr 的 snapshot 当前是**本地文件**:

- `--snapshot-path` 配置一个绝对路径,周期 + dirty flush 写入。
- envelope 校验 / `.bak` 回退 / 原子替换都建立在 POSIX 文件系统语义上。
- Reviver 拉起新 mgr 时,新 mgr 从同一路径加载 snapshot。

这意味着 **Reviver 和被监督 mgr 必须能访问到同一份文件**。
F1(machined-backed lease)已经解决了 leader lock 跨机问题:Reviver 可以
跨主机竞争领导权。但 snapshot 跨机仍是空白:

- Reviver A 在主机 1,持锁、监督 CellAppMgr;
- CellAppMgr 在主机 1 crash;
- Reviver A 被 kill,Reviver B 在主机 2 接管 leader lock;
- Reviver B 想拉起新 CellAppMgr,但**主机 2 没有主机 1 的 snapshot
  文件**,新 mgr 启动后丢掉了 BSP / cellapp registry 等权威状态,只能
  等所有 CellApp / BaseApp 重新注册 — 失去了 reattach 带来的快速收敛。

要让 Phase 13 的 Reviver 在真实跨机场景下保留 BigWorld 风格的 "mgr 持久
化 + reattach" 能力,snapshot 必须放在**任何一台 Reviver 候选机都能访问
的位置**。本 RFC 评估几条路径并给出建议。

## 2. 现状回顾

`snapshot_envelope.h` 的契约:

```
WrapPayload(payload, magic, version)  // 序列化端
ReadPayload(bytes, magic, version, max_payload_bytes, module_name)
PreserveBackup(path, ...)  // 写 .bak 前验证主文件
Readiness(path, validate, ...)  // 文件健康度
```

操作单位都是**整文件覆盖** — 写时先 `WriteFile(tmp)` 然后
`AtomicReplaceFile(tmp, path)`。这是为单机本地 FS 设计的:依赖 rename
在同一目录下的原子性、`.bak` 由 ops 兜底 corrupt 情况。

snapshot 体积:

| Mgr | 当前实测 | 上限 |
|---|---|---|
| CellAppMgr | ~1 KB (空 4-leaf space) → ~50 KB (200 cellapp / 多 space) | `kMaxSnapshotPayloadBytes = 1 GiB` |
| BaseAppMgr | ~200 B (1 BaseApp) → ~10 KB (100 BaseApp) | `kMaxSnapshotPayloadBytes = 256 MiB` |

落盘频率:`--snapshot-interval-ms`(默认 1s),dirty 触发额外加速到 1s 节
流。每秒写一个几十 KB 的文件,对任何后端都不是吞吐瓶颈;关注的是**延迟
+ 一致性**。

## 3. 候选方案

### 3.1 NFS / SMB 共享卷

每台 Reviver 候选机挂载同一个网络共享卷,`--snapshot-path` 指向卷内路径。

**优点**:
- 0 代码改动 — `WriteFile + AtomicReplaceFile` 在 NFSv3+ / SMB3 上仍工作。
- 现有 envelope / `.bak` 机制 100% 复用。
- 运维链路成熟。

**缺点**:
- NFS 的 `rename` 原子性需要 close-to-open 一致性 — 跨客户端可见性可能
  有数十毫秒延迟。BaseAppMgr 接管时刚 rename 完的新 snapshot,Reviver B
  的本地缓存可能仍看到旧内容。
- 文件锁(`flock` / `LockFileEx`)在 NFS/SMB 上的语义不可靠。F1 已经把
  leader lock 从文件锁迁到 machined-lease,但如果 ops 仍想用 file-lock
  mode + NFS 共享卷,会踩坑。需要在 Phase 13 文档明确禁止。
- 单点 — NFS server 宕机时 Reviver 和 mgr 都失去 snapshot 访问。

**建议适用场景**:小型部署 + ops 已经维护 NFS 集群,愿意接受 "snapshot
freshness 滞后 100ms 级别"。

### 3.2 对象存储(S3 / GCS / Azure Blob)

每次 `SaveSnapshotToFile` 改成 PUT 到对象存储 key,`RestoreSnapshotFromFile`
改成 GET。`.bak` 用对象 versioning 替代。

**优点**:
- 强一致性(S3 自 2020 起 read-after-write 强一致)。
- 99.99%+ SLA — Reviver 候选机不需要本地挂载任何东西。
- 历史版本天然保留(versioning),`.bak` 不再需要本地维护。

**缺点**:
- 延迟数十-数百 ms 每次 PUT — 1Hz snapshot 在 dirty 路径上变成可见的
  control-plane 延迟。需要从同步改成 background save。
- 引入第三方依赖(aws-sdk / curl + S3 协议)— 与 Atlas "C++ 标准库 +
  stdlib Python tooling" 的极简风格冲突。
- 单元测试需要 minio / mock S3 服务,CI 复杂度上升。
- 凭证管理(AWS keys / IAM role)— 新增运维面。

**建议适用场景**:多机房多 Reviver 部署 + 已有云厂商账号 + ops 接受云
依赖。生产推荐方案。

### 3.3 etcd / consul KV

把整个 snapshot 字节流写到 KV 的一个 key 下,`.bak` 写到 `<key>/bak`。

**优点**:
- 与 F1 风格一致(F1 用 machined 做 lease,etcd 做 KV 是同类思路)。
- 强一致性。
- 改造工作量类似 S3。

**缺点**:
- 单值大小限制:etcd 默认 1.5 MiB,CellAppMgr snapshot 上限 1 GiB,得
  改成 sharded keys 或限制 snapshot 大小。
- 引入 etcd 部署依赖,集群规模 / 维护成本不低。
- atlas 项目还没引入 etcd,新依赖。

**建议适用场景**:已经在跑 k8s + etcd 的部署,愿意把 KV 当 blob store 用。

### 3.4 WAL append-only(本地或共享)

不再保存"整文件",改成 append-only:每次 dirty 把 mutation 追加到日志。
Restore = replay 整个 WAL。周期 checkpoint 把 WAL 截断。

**优点**:
- 写延迟低(append 而不是 rename)。
- 自然支持增量复制 — WAL tail 可以被 ship 到 backup,基本 free。
- crash safety 强 — 即使写到一半,replay 时跳过末尾不完整记录即可。

**缺点**:
- **必须重写 snapshot 协议**:从 "整状态序列化" 改成 "mutation log"。
  每个 mutation 类型独立 ID、独立 schema 版本。这是从头设计一个微型
  存储引擎,工作量 ~3000+ 行 + 测试。
- replay 速度问题:cellappmgr 几小时累积的 WAL 可能要几秒 replay。需要
  周期 checkpoint(本质回到 full snapshot)。
- snapshot_envelope.h 当前抽象不适用 — WAL 需要新的 envelope(per-record
  magic + checksum),已有的 mgr 改造成本高。

**建议适用场景**:Phase 14+ 的高吞吐场景(>10 Hz dirty mutation)。当前
1 Hz snapshot 用不上 WAL 的延迟优势。

### 3.5 数据库表(用 DBApp / Postgres)

把 snapshot 当 BLOB 写进数据库表 `mgr_snapshots(key, payload, checksum,
written_at)`。

**优点**:
- 复用 Phase 7 已有的 DBApp 基础设施(SQLite / MySQL backend),不引入
  新依赖。
- 跨机访问通过 DBApp 网络层。
- 历史版本天然存在(insert 而不是 update)。

**缺点**:
- 循环依赖:CellAppMgr / BaseAppMgr 都依赖 DBApp,而 DBApp 本身也有 HA
  需求(就是 Phase 13 的 #2 follow-up)。DBApp 没起或被 kill 时 mgr 拿
  不到 snapshot,变成串行依赖。
- DBApp 自己的 snapshot 怎么处理 — 鸡生蛋问题。
- MySQL/SQLite 都不适合 100KB-MB 量级的 BLOB 高频写。

**建议适用场景**:不建议。循环依赖是 deal-breaker。

## 4. 建议路线

按当前 Atlas 部署形态(单机开发 / 单数据中心)分阶段:

**P1(当前,Phase 13 follow-up)**:保持本地文件 + 文档说明跨机限制。
本 RFC 作为 trade-off 归档,不立即实现。

**P2(跨机部署需求出现时,~Phase 17 ?)**:NFS 共享卷,接受 100ms 级
缓存延迟。代码 0 改动,只新增 ops 部署文档。

**P3(多数据中心 / SLA 要求)**:S3 兼容对象存储,改造
`SaveSnapshotToFile` / `RestoreSnapshotFromFile`,引入可配的 storage
backend 抽象。其他 mgr 通过 `snapshot_envelope.h` 自动受益。

## 5. snapshot_envelope.h 的扩展点

如果选 P3(S3 后端),需要 `snapshot_envelope.h` 长出一个 storage 抽象:

```cpp
namespace atlas::snapshot_envelope {

// 现在: 函数直接走 fs::Read/WriteFile
// 改造后: 通过一个 Backend 接口
class Backend {
 public:
  virtual auto Read(std::string_view key) -> Result<std::vector<std::byte>> = 0;
  virtual auto Write(std::string_view key, std::span<const std::byte>) -> Result<void> = 0;
  virtual auto AtomicReplace(std::string_view tmp_key, std::string_view final_key)
      -> Result<void> = 0;
  virtual auto Exists(std::string_view key) -> bool = 0;
  virtual auto Size(std::string_view key) -> Result<uint64_t> = 0;
};

// 默认实现包当前 platform/filesystem.h 的语义,新后端实现 S3 / NFS 等。
class FilesystemBackend : public Backend;
class S3Backend : public Backend;

// Readiness / PreserveBackup / WrapPayload / ReadPayload 全部接受 Backend&,
// 实际文件路径变成 key。
}
```

各 mgr 通过 ServerConfig 选 backend:

```
--snapshot-backend filesystem  (default; preserves current behaviour)
--snapshot-backend s3 --snapshot-s3-bucket atlas-ha --snapshot-s3-region us-east-1
```

这个抽象层是**未来工作**,本 RFC 不要求现在实现。把它写在这里只是为了
说明:**抽 snapshot_envelope 时已经为后端切换留好了接口边界**(整个
load / save / readiness / backup 都已收口在 envelope 命名空间内,改造
集中在一处)。

## 6. 不会做

- 实现 etcd / consul backend — 与 Atlas 极简依赖原则冲突。
- 实现 WAL / mutation log — snapshot 体积和频率都不需要。
- 把 snapshot 塞进 DBApp — 循环依赖。
- 任何要求引入第三方 C++ SDK 的方案 — 短期内不做(Phase 13 follow-up
  范畴)。

## 7. 与其他 Phase 的边界

- **Phase 13 F1(machined-lease)**:F1 解决了 lock 跨机,本 RFC 解决了
  state 跨机。两者独立可选 — local file lock + local snapshot 是单机
  开发组合;machined-lease + S3 backend 是多机生产组合。
- **lease fencing token**:`mgr_generation` epoch 只防在途旧包,不防过期
  leader 启动一个更高 generation 的 mgr(见 phase13_high_availability.md
  "防护边界")。真正的跨 leader fencing(machined Acquire 返回单调 fence →
  穿到 mgr 启动参数 → generation 受其约束)只在多机 / 分区下有意义,且依赖
  本 RFC 选定的跨机仲裁面(复制 machined / raft 才能提供单调且高可用的
  fence)。因此 fencing 与本 RFC 的后端选型一并决策,不单独实现。
- **Phase 15 DBAppMgr**(见 `phase15_dbappmgr.md`):DBAppMgr 的 snapshot
  也走同一个 envelope,这里描述的 backend 切换对它直接生效。

## 8. 验证基线(if 实现 P2 / P3)

如果未来实现 NFS / S3 backend,需要新增的 live 验证:

- `verify_cellappmgr_ha --check-snapshot-backend filesystem|nfs|s3` —
  声明当前 backend,跨 Reviver host 测试 takeover 后 restore 成功。
- summary JSON 加 `current.snapshot.backend`(已有 `path`),区分本地
  vs 共享存储下的 restore 路径。
- live verify 矩阵在跨主机 cluster 上跑 BaseAppMgr / CellAppMgr 双 mgr
  同时 takeover,确认两个 Reviver 候选机都能拿到新鲜 snapshot。

这些都在 backend 抽象落地后再补,本 RFC 不展开。
