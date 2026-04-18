# ScriptPhase 2: C++ ↔ C# 互操作层

> 预估周期: 4–5 周 | 前置依赖: ScriptPhase 1 完成 + [NativeApi 架构基础设施](native_api_architecture.md) 中的方案 A (共享库) 和方案 B (Provider) 就位 | **状态: ✅ 完成**

---

## 目标

1. 建立高效的双向互操作层：C++ 可调用 C# 方法，C# 可调用 C++ 函数。
2. 实现类型编组（Marshal）—— blittable 类型零拷贝，字符串/字节数组高效传递。
3. 实现托管对象句柄管理（GCHandle）。
4. 建立 C# → C++ 的 `[LibraryImport]` 通道（原计划的 `Atlas.Generators.Interop` 已裁掉，见下文）。
5. 建立异常桥接机制。

## 验收标准 (M2)

- [x] C++ 可调用 C# 静态方法并获取正确返回值 — `ClrStaticMethod<Ret, Args...>` + `test_clr_invoke`。
- [x] C# 可通过 `[LibraryImport]` 调用 C++ 导出函数 — `NativeApi.cs` + `test_clr_callback`。
- [x] 支持 blittable 类型、字符串、`byte[]` 的双向传递 — `clr_marshal.h/cc` + `test_clr_marshal`。
- [x] C# 异常能被 C++ 侧捕获并转换为 `Error` — `ErrorBridge` + `ClrErrorBuffer` TLS + `test_clr_error`。
- [x] `GCHandle` 生命周期管理正确，无泄漏 — `ClrObjectVTable` + `GCHandleTracker` + `test_clr_object`。

> 实现过程中发现的 DLL TLS 隔离、CLR 双 Assembly 实例、Source Generator 链式限制等关键问题，详见 [implementation_notes.md](implementation_notes.md)。

## 实际落地（2026-04-18 核对）

| 关注点 | C++ 侧 | C#/测试侧 |
|--------|--------|-----------|
| 共享库输出 | `add_library(atlas_engine SHARED ...)` → `build/debug/src/lib/clrscript/Debug/atlas_engine.dll` | `NativeApi.cs` 中 `[LibraryImport("atlas_engine")]` |
| 类型编组 | `clr_marshal.h/cc`（blittable struct / `ClrStringRef` / `ClrSpanRef` / `ClrVector3` / `ClrQuaternion`） | C# 结构体与 `SpanReader/Writer` 已对齐 |
| GCHandle 对象 | `clr_object.h/cc`（move-only）+ `clr_object_registry.h/cc`（热重载前强制释放） | `GCHandleHelper`（Atlas.Runtime）+ `GCHandleTracker`（Debug） |
| C++ → C# 调用 | `clr_invoke.h`（`ClrStaticMethod<Ret, Args...>` 模板 wrapper） | `[UnmanagedCallersOnly]` 入口 |
| C# → C++ 调用 | `clr_native_api.h/cc`（展开自 `clr_native_api_defs.h` X-macro） | `NativeApi.cs` 中 `[LibraryImport]` 方法 + `fixed` 包装 |
| Provider 差异化 | `native_api_provider.h/cc` + `base_native_provider.h/cc` | BaseApp 特化在 `baseapp_native_provider.*` |
| 异常桥接 | `clr_error.h/cc`（`thread_local ClrErrorBuffer`） | `ErrorBridge.cs`（静态 `delegate* unmanaged` 字段，由 Bootstrap 注入） |
| 导出宏 | `clr_export.h`（`ATLAS_NATIVE_API = extern "C" + ATLAS_EXPORT`） | — |

### 测试矩阵（合计 ~116 C++ 用例）

| 测试文件 | 用例数 | 范围 |
|---------|--------|------|
| `test_clr_marshal.cpp` | 41 | blittable 布局/偏移、字符串/`byte[]` 往返、`ScriptValue` 全类型 |
| `test_clr_object.cpp` | 21 | GCHandle 分配/释放、move、vtable、错误传播、Debug 泄漏计数 |
| `test_clr_invoke.cpp` | 12 | 绑定/调用、`int` error convention、`void`/`float` 返回值、move |
| `test_clr_callback.cpp` | 6 | CLR 集成：ABI 版本、LogMessage、ServerTime、异常传播、GCHandle 生命周期、ProcessPrefix |
| `test_clr_error.cpp` | 12 | TLS 读写、截断、线程隔离、clear/get_code |
| `test_native_api_provider.cpp` | 14 | Provider 注册/获取、`BaseNativeProvider` 默认行为 |
| `test_native_api_exports.cpp` | 10 | `atlas_engine.dll` 符号导出验证（`dlsym` / `GetProcAddress`） |

## 关键架构决策

1. **引擎核心构建为共享库 `atlas_engine.dll/.so`**：C# 的 `[LibraryImport("atlas_engine")]` 走标准 DLL 查找流程，不需要 `DllImportResolver` hack。详见 [native_api_architecture.md](native_api_architecture.md) 方案 A。
2. **`ATLAS_NATIVE_API_TABLE` X-macro 为单一定义源**：`src/lib/clrscript/clr_native_api_defs.h` 同时展开声明、实现、`INativeApiProvider` 纯虚方法、`BaseNativeProvider` 默认实现四处。新增 API 只需编辑该头文件。
3. **`ClrObjectVTable` 在 Bootstrap 时一次性注入**：7 个函数指针（`free_handle / get_type_name / is_none / to_int64 / to_double / to_string / to_bool`）由 C# `GCHandleHelper` 的 `[UnmanagedCallersOnly]` 方法地址填充。所有 `ClrObject` 共享同一份 vtable，方法调用是 O(1) 间接跳转，无需每次 `get_method` 查找。
4. **错误桥走 C++ TLS**：C# `ErrorBridge` 不使用 `[ThreadStatic]`，而是通过 `ClrBootstrapArgs` 注入 C++ 侧 `clr_error_set/clear/get_code` 的函数指针。DLL 与 EXE 的 TLS 隔离问题通过 DLL 导出 `AtlasGetClrError*Fn()` 查询函数解决（详见 [implementation_notes.md](implementation_notes.md) §1）。
5. **Provider 模式支持进程差异化**：每个服务端可执行文件（BaseApp / CellApp / DBApp / Reviver）注册自己的 `INativeApiProvider` 实现；`Atlas*` 导出函数在 C++ 侧统一委托给当前 provider。

## 已裁掉的方向

- **`Atlas.Generators.Interop`**：原计划由自定义 Source Generator 从 `[NativeImport]` 生成 `[LibraryImport]` 声明和 Span wrapper。实现后发现：
  - Roslyn 链式限制：我们生成的 `[LibraryImport] partial` 方法不会被官方 `LibraryImportGenerator` 再次处理（CS8795）。
  - Custom Marshaller 是 1:1 参数映射，无法把 `ReadOnlySpan<byte>` 展开为 `(byte*, int)` 两个原生参数。
  - 当前 `atlas_*` 导出函数 < 15 个，手写成本远低于自定义 Generator 的基础设施开销。
  
  结论：保留手写的 `NativeApi.cs`，使用官方 `[LibraryImport]` + `fixed` Span 包装。详见 [implementation_notes.md](implementation_notes.md) §3。

## 一致性保证

- C++ 和 C# 函数签名靠 `test_native_api_exports.cpp`（`dlsym` / `GetProcAddress`）验证所有 `atlas_*` 符号已导出。
- 结构体大小与对齐通过 C++ `static_assert` + C# `Debug.Assert(sizeof(...))` 双侧自检。
- 后续若 `atlas_*` 函数数量超过 20 个，可引入 YAML → C++ header / C# partial class 代码生成；当前阶段不需要。
