# ScriptPhase 6: 测试与稳定化

> 预估周期: 3–4 周 | 前置依赖: ScriptPhase 4 + ScriptPhase 5 完成 | **状态: 🟡 进行中**

---

## 目标

1. 完善全部测试覆盖（C++ 单元测试 + C# xUnit + 集成测试 + 压力测试）。
2. GC 调优，确保游戏服务器场景下暂停时间可控。
3. 建立性能基准，对标 Python 方案数据。
4. 完成技术文档。

## 验收标准 (M6)

- [~] 全部测试通过。
- [ ] 10,000 实体压测：连续 1 小时无内存泄漏。
- [ ] GC 暂停 p99 < 5ms。
- [ ] C++ ↔ C# 单次调用延迟 < 100ns（blittable）、< 500ns（string）。
- [~] 开发者文档完整。

---

## 当前测试资产（2026-04-18 核对）

### C++ 脚本层单元测试（`tests/unit/`）

| 文件 | 覆盖 |
|------|------|
| `test_script_value.cpp` | `ScriptValue` 全类型构造/提取/move |
| `test_script_events.cpp` | `ScriptEvents` 基于 Mock `ScriptObject` 的生命周期回调与事件注册 |
| `test_script_app.cpp` | `ScriptApp` 启动关闭 |
| `test_clr_host.cpp` | `ClrHost::Initialize / GetMethod / Finalize` + 错误场景 |
| `test_clr_marshal.cpp` | 41 用例：blittable 结构体布局/偏移、字符串、`byte[]` |
| `test_clr_object.cpp` | 21 用例：`ClrObject` GCHandle 生命周期、move、vtable、Debug 泄漏计数 |
| `test_clr_invoke.cpp` | 12 用例：`ClrStaticMethod::Bind / Invoke / Reset`，`int` error convention，`void/float` 返回值 |
| `test_clr_callback.cpp` | 6 用例：ABI 版本、`LogMessage`、`ServerTime`、异常传播、GCHandle 生命周期、`ProcessPrefix` |
| `test_clr_error.cpp` | 12 用例：TLS 缓冲区读写/截断/线程隔离 |
| `test_clr_script_engine.cpp` | `ClrScriptEngine` 完整生命周期 |
| `test_native_api_provider.cpp` | 14 用例：`INativeApiProvider` 注册/获取、`BaseNativeProvider` 默认行为 |
| `test_native_api_exports.cpp` | 10 用例：`atlas_engine.dll` 符号导出验证 |

### C# 测试

| 测试项目 | 文件 | `[Fact]` 数量 |
|----------|------|---------------|
| `Atlas.Runtime.Tests` | `DirtyTrackingTests.cs` | 12 |
| | `EntityFactoryTests.cs` | 8 |
| | `EntityManagerTests.cs` | 20 |
| | `EntitySerializationTests.cs` | 16 |
| | `ScriptHostTests.cs` | 8 |
| | `SpanWriterReaderTests.cs` | 38 |
| `Atlas.Generators.Tests` | `DefGeneratorTests.cs` | DefGenerator 属性/序列化/Mailbox/Dispatcher/RPC ID/诊断 |
| 辅助程序集 | `Atlas.RuntimeTest/CallbackEntryPoint.cs`、`LifecycleTestEntryPoint.cs`；`Atlas.SmokeTest/EntryPoint.cs` | C++ 集成测试使用的 forwarder / 冒烟入口 |

---

## 缺口（M6 无法关闭的项）

| 项 | 状态 | 说明 |
|----|------|------|
| `test_hot_reload.*` | 未开始 | Phase 5 收尾时补齐：编译失败回滚、连续重载、状态迁移、leak_count == 0 |
| `test_engine_lifecycle.*` | 未开始 | C++ 启动 → C# 初始化 → Tick N 帧 → 关闭 端到端 |
| `test_rpc_dispatch.*` / `test_event_system.*` | 未开始 | DefGenerator / EventGenerator 产物的运行时覆盖 |
| 10K 实体稳定性压测 | 未开始 | 1 小时持续 Tick，监控 RSS / GC 暂停 |
| `BenchmarkDotNet` 基准 | 未开始 | 调用延迟、序列化、工厂、RpcDispatch、10K Tick |
| 开发者文档 | 未开始 | `quickstart.md` / `api_reference.md` / `source_generators.md` / `hot_reload_guide.md` / `unity_integration.md` / `architecture.md` / `performance.md` |
| CI 集成 | 未开始 | `ATLAS_ENABLE_CLR=ON` + dotnet + CTest + 覆盖率（gcov / coverlet） |

## GC 调优路径

`runtime/atlas_server.runtimeconfig.json` 已启用：`Server GC` / `Concurrent` / `RetainVM` / `DynamicAdaptationMode=1` / 分层编译。后续探索项：

- 识别热路径（`OnTick`、RPC 收发）堆分配，评估对象池化。
- `Span<T>` / `stackalloc` 替代堆分配（`Atlas.Log.Send` 已采用）。
- `FrozenDictionary` 用于 `EntityFactory` 构造器表与 `RpcDispatcher` 路由。
- `GC.TryStartNoGCRegion` 在关键帧中的可行性评估。
