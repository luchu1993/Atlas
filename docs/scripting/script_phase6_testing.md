# ScriptPhase 6: 测试与稳定化

> 预估周期: 3–4 周 | 前置依赖: ScriptPhase 4 + ScriptPhase 5 完成

---

## 目标

1. 完善全部测试覆盖（C++ 单元测试 + C# xUnit + 集成测试 + 压力测试）
2. GC 调优，确保游戏服务器场景下暂停时间可控
3. 建立性能基准，对标 Python 方案数据
4. 完成技术文档

## 验收标准 (M6)

- [~] 全部测试通过（C++ ~80 用例, C# ~60 用例, 集成 ~15 用例）
- [ ] 10,000 实体压测: 连续 1 小时无内存泄漏
- [ ] GC 暂停 p99 < 5ms
- [ ] C++ ↔ C# 单次调用延迟 < 100ns (blittable), < 500ns (string)
- [~] 文档完整，可供新团队成员参考

## 验收状态（2026-04-13）

- 当前仓库已经具备较完整的测试骨架: `tests/unit/test_clr_*`、`test_script_*`、`tests/csharp/Atlas.Runtime.Tests`、`tests/csharp/Atlas.Generators.Tests`。
- 这说明“测试矩阵建设”已经启动并有实际沉淀，但距离 `M6` 需要的全量通过、长稳压测与性能门槛仍有明显差距。

---

## 任务 6.1: C++ 单元测试完善

### 测试矩阵

| 模块 | 测试文件 | 用例数 | 重点 |
|------|---------|--------|------|
| ScriptValue | `test_script_value.cpp` | ~15 | 类型构造/提取/move/copy/类型错误 |
| ScriptEvents | `test_script_events.cpp` | ~8 | Mock 对象生命周期回调/事件注册 |
| ClrHost | `test_clr_host.cpp` | ~10 | 初始化/关闭/方法加载/错误场景 |
| ClrMarshal | `test_clr_marshal.cpp` | ~12 | blittable 往返/string/bytes/边界值 |
| ClrObject | `test_clr_object.cpp` | ~10 | GCHandle 生命周期/move/泄漏检测 |
| ClrInvoke | `test_clr_invoke.cpp` | ~8 | 绑定/调用/错误处理/void 返回 |
| ClrCallback | `test_clr_callback.cpp` | ~6 | 函数表传递/版本检查/回调正确性 |
| ClrError | `test_clr_error.cpp` | ~6 | 异常桥接/嵌套异常/截断/清除 |
| ClrScriptEngine | `test_clr_script_engine.cpp` | ~8 | 完整生命周期/模块加载/错误 |
| **合计** | | **~83** | |

### 新增边界用例

| 测试 | 验证内容 |
|------|---------|
| `ClrMarshal_EmptyString` | 空字符串传递不崩溃 |
| `ClrMarshal_UnicodeString` | 中文/emoji UTF-8 正确传递 |
| `ClrMarshal_LargeString` | 1MB 字符串传递 |
| `ClrMarshal_NullPointer` | null 指针处理 |
| `ClrObject_DoubleRelease` | 防止双重释放 GCHandle |
| `ClrObject_MoveFromReleased` | 从已释放对象 move 不崩溃 |
| `ClrHost_ConcurrentGetFunction` | 多线程同时获取函数指针 |
| `ClrScriptEngine_ReInitialize` | finalize 后重新 initialize |

---

## 任务 6.2: C# xUnit 测试完善

### 项目: `tests/csharp/Atlas.Runtime.Tests/`

| 测试类 | 用例数 | 重点 |
|--------|--------|------|
| `LogTests` | ~4 | Log.Info/Warning/Error/Critical 调用 NativeApi |
| `GameTimeTests` | ~3 | ServerTime/DeltaTime 属性读取 |
| `EntityManagerTests` | ~10 | Create/Destroy/OnTickAll/OnInitAll/OnShutdownAll |
| `EntitySerializationTests` | ~8 | Serialize/Deserialize 往返/版本兼容/边界值 |
| `DirtyTrackingTests` | ~6 | 设置属性→IsDirty/SerializeDelta/ClearDirty |
| `EntityFactoryTests` | ~4 | Create by name/不存在类型/注册完整性 |
| `SpanWriterReaderTests` | ~12 | 各类型写入/读取/空值/大数据 |
| `ScriptHostTests` | ~4 | Load/Unload/WeakReference 验证 |
| **合计** | **~51** | |

### Source Generator 测试

| 测试类 | 用例数 | 重点 |
|--------|--------|------|
| `InteropGeneratorTests` | ~8 | 结构体生成/方法生成/版本检查/诊断 |
| `EntityGeneratorTests` | ~10 | 序列化/脏标记/工厂/诊断 |
| `RpcGeneratorTests` | ~8 | 存根/分发/ID 分配/诊断 |
| `EventGeneratorTests` | ~6 | 注册/注销/参数类型/诊断 |
| **合计** | **~32** | |

---

## 任务 6.3: 集成测试

### 测试列表

| 测试文件 | 描述 | 涉及组件 |
|---------|------|---------|
| `test_engine_lifecycle.cpp` | C++ 启动 → C# 初始化 → Tick 100帧 → 关闭 | ClrScriptEngine, Bootstrap |
| `test_entity_creation.cpp` | C++ 请求创建实体 → C# EntityFactory 生成 → 回调 OnInit | Entity 全链路 |
| `test_log_roundtrip.cpp` | C# Log.Info → NativeApi → C++ ATLAS_LOG_INFO | 日志绑定 |
| `test_time_binding.cpp` | C# Atlas.Time.ServerTime 读取 → 值正确 | 时间绑定 |
| `test_entity_serialization.cpp` | 创建实体 → 设置属性 → 序列化 → 反序列化 → 比对 | Source Generator |
| `test_dirty_sync.cpp` | 设置属性 → IsDirty → SerializeDelta → Apply → 验证 | 脏标记 |
| `test_rpc_dispatch.cpp` | 发送 RPC → 序列化 → 分发 → 接收回调 | RPC Generator |
| `test_event_system.cpp` | 注册事件 → 触发 → 回调 → 注销 | Events Generator |
| `test_hot_reload.cpp` | 加载 → 调用 → 修改 → 重载 → 验证新逻辑 | 热重载全链路 |
| `test_hot_reload_state.cpp` | 创建实体 → 设置状态 → 重载 → 验证状态恢复 | 状态迁移 |
| `test_gchandle_leak.cpp` | 大量 ClrObject 创建/销毁 → leak_count == 0 | GCHandle 管理 |
| `test_concurrent_calls.cpp` | 多线程 C++ 调用 C# → 结果正确/无竞态 | 线程安全 |
| **合计** | **~12** | |

---

## 任务 6.4: 压力测试

### 6.4.1: 大规模实体 Tick

```
场景: 10,000 个 PlayerEntity, 每帧 OnTick
目标:
  - OnTickAll 耗时 < 2ms (单线程)
  - 内存稳定（1小时内增长 < 10MB）
  - GC 暂停 p99 < 5ms

测量方式:
  - C++ 侧计时: on_tick() 调用前后 Clock::now()
  - C# 侧 GC 统计: GC.GetGCMemoryInfo() 每 60 秒采样
  - 进程 RSS 监控: 每分钟记录
```

### 6.4.2: 跨边界调用吞吐

```
场景: 单线程循环调用 C++ → C# → 返回, 不同参数类型
目标:
  - blittable (int → int): < 100ns/call
  - string (短): < 500ns/call
  - string (1KB): < 2μs/call
  - byte[] (1KB): < 1μs/call

测量方式:
  - 100万次调用取平均
  - 排除 JIT 预热 (忽略前 10,000 次)
```

### 6.4.3: 长时间稳定性

```
场景: 模拟真实游戏服务器负载
  - 1000 个实体持续 Tick
  - 每秒 100 次 RPC 调用
  - 每 5 分钟触发一次热重载
  - 持续运行 24 小时

目标:
  - 无崩溃
  - 无内存泄漏 (RSS 增长 < 50MB/24h)
  - GCHandle leak_count == 0
  - 热重载成功率 100%
```

---

## 任务 6.5: GC 调优

### 基线配置 (runtimeconfig.json)

```json
{
  "configProperties": {
    "System.GC.Server": true,
    "System.GC.Concurrent": true,
    "System.GC.RetainVM": true
  }
}
```

### 调优策略

| 手段 | 说明 | 适用场景 |
|------|------|---------|
| Server GC | 每个 CPU 独立 GC 堆，减少锁竞争 | 始终启用 |
| Concurrent GC | 后台标记阶段并发，减少 STW | 始终启用 |
| `GC.TryStartNoGCRegion()` | 临时禁止 GC（预分配足够内存） | 关键帧处理期间 |
| 对象池 | 高频临时对象复用（Entity, RPC buffer） | OnTick 内部 |
| `Span<T>` / `stackalloc` | 栈分配替代堆分配 | 短生命周期 buffer |
| `FrozenDictionary` | 只读字典，减少 GC 扫描 | 实体工厂、RPC 路由表 |

### 监控代码

```csharp
internal static class GCMonitor
{
    private static Timer? _timer;

    public static void Start(TimeSpan interval)
    {
        _timer = new Timer(_ => Report(), null, interval, interval);
    }

    private static void Report()
    {
        var info = GC.GetGCMemoryInfo();
        Atlas.Log.Info($"GC: heap={info.HeapSizeBytes / 1024 / 1024}MB " +
                       $"gen0={GC.CollectionCount(0)} " +
                       $"gen1={GC.CollectionCount(1)} " +
                       $"gen2={GC.CollectionCount(2)} " +
                       $"pause={info.PauseTimePercentage:F2}%");
    }
}
```

### 工作点

- [ ] 基线测试: 默认配置下的 GC 表现
- [ ] 调整 `GCLatencyMode` 比较: `Interactive` vs `SustainedLowLatency`
- [ ] `GC.TryStartNoGCRegion()` 在 OnTick 中的可行性测试
- [ ] 识别热路径上的堆分配（使用 `dotnet-counters` 或 `BenchmarkDotNet`）
- [ ] 对象池化策略: 确定哪些对象值得池化

---

## 任务 6.6: 性能基准

### 基准测试框架

C# 侧使用 `BenchmarkDotNet`，C++ 侧使用 Google Benchmark 或手动计时。

### 基准项

| 项目 | 测量指标 | 目标 |
|------|---------|------|
| C++ → C# 调用 (int) | ns/op | < 100 |
| C++ → C# 调用 (string 短) | ns/op | < 500 |
| C++ → C# 调用 (string 1KB) | ns/op | < 2000 |
| C# → C++ 回调 (int) | ns/op | < 80 |
| Entity.Serialize (5 属性) | ns/op | < 200 |
| Entity.Deserialize (5 属性) | ns/op | < 200 |
| DirtyTracking.SerializeDelta | ns/op | < 100 |
| EntityFactory.Create | ns/op | < 50 |
| RpcDispatch (单次) | ns/op | < 300 |
| 10K Entity OnTick | ms/frame | < 2 |

### 与 Python 对比数据 (预期)

| 操作 | CPython 3.12 | .NET 9 CLR | 提升倍数 |
|------|-------------|-------------|---------|
| 函数调用开销 | ~200ns | ~50ns | ~4x |
| 属性访问 | ~100ns | ~5ns | ~20x |
| 序列化 (5 字段) | ~5μs | ~200ns | ~25x |
| 10K Entity Tick | ~50ms | ~2ms | ~25x |
| 内存/实体 | ~2KB | ~200B | ~10x |

---

## 任务 6.7: 文档

### 新建文档

| 文件 | 内容 | 目标读者 |
|------|------|---------|
| `docs/scripting/quickstart.md` | 从零开始写第一个 C# 服务端脚本 | 游戏开发者 |
| `docs/scripting/api_reference.md` | Atlas.Runtime C# API 完整参考 | 游戏开发者 |
| `docs/scripting/source_generators.md` | Source Generator 使用指南 + 属性标记说明 | 游戏开发者 |
| `docs/scripting/hot_reload_guide.md` | 热重载使用说明 + 限制 + 故障排除 | 游戏开发者 |
| `docs/scripting/unity_integration.md` | Unity 客户端集成 Atlas.Shared | Unity 开发者 |
| `docs/scripting/architecture.md` | 技术架构 + 设计决策记录 (ADR) | 引擎开发者 |
| `docs/scripting/performance.md` | 性能调优指南 + 基准数据 | 运维/引擎开发者 |

### quickstart.md 大纲

```
1. 环境准备
   - 安装 .NET 9 SDK
   - 配置 CMake (ATLAS_ENABLE_CLR=ON)
   - 构建引擎

2. 创建第一个实体
   - 编写 PlayerEntity.cs
   - 添加 [Entity], [Replicated] 标记
   - 编译并运行

3. 添加逻辑
   - 重写 OnInit / OnTick
   - 使用 Atlas.Log / Atlas.Time
   - 调试和热重载

4. 添加 RPC
   - [ServerRpc] / [ClientRpc]
   - 客户端集成

5. 最佳实践
   - 避免堆分配
   - 使用 Span<T>
   - 合理使用热重载
```

---

## 任务 6.8: CI/CD 集成

### 工作点

- [ ] CI 流水线安装 .NET 9 SDK
- [ ] CMake 配置: `ATLAS_ENABLE_CLR=ON`
- [ ] 构建步骤: `dotnet build` C# 项目 → CMake build C++ → CTest
- [ ] Source Generator 编译验证: C# 项目构建成功即验证 Generator 输出
- [ ] 测试执行: C++ Google Test + C# xUnit + 集成测试
- [ ] 覆盖率收集: C++ (gcov/llvm-cov), C# (coverlet)

---

## 任务依赖图

```
6.1 C++ 单元测试 ─────────┐
                           │
6.2 C# xUnit 测试 ────────┤
                           │
6.3 集成测试 ──────────────┤
                           │
6.4 压力测试 ──────────────┤
                           │
6.5 GC 调优 ──────────────┤
                           ├── 6.8 CI/CD
6.6 性能基准 ──────────────┤
                           │
6.7 文档 ─────────────────┘
```

**大部分任务可并行**。建议顺序: 6.1 + 6.2 → 6.3 → 6.4 + 6.5 (并行) → 6.6 → 6.7 + 6.8
