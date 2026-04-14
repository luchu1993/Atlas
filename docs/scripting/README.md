# Atlas Engine — C# 脚本层迁移规划

> 版本 1.0 | 目标运行时: .NET 9 | 策略: 全面放弃 Python，Source Generator 零反射

---

## 1. 目标

将 Atlas Engine 的服务端脚本语言从 **Python 3** 完全迁移到 **C# (.NET 9)**，实现：

1. 服务端与 Unity 客户端通过 `Atlas.Shared` 共享实体定义和业务逻辑
2. 深度使用 C# Source Generator，**零反射、零动态代码生成**，完全兼容 Unity IL2CPP
3. 保持引擎侧干净的 `ScriptEngine` 抽象接口（便于测试和未来扩展）
4. 支持开发期 C# 程序集热重载
5. 性能优于 CPython，GC 暂停可控（Server GC + DATAS）

## 2. 核心架构决策

### 2.1 为什么选择 .NET 9

| 特性 | 收益 |
|------|------|
| Source Generator (IIncrementalGenerator) | 编译期生成绑定/序列化/RPC 代码，零运行时反射 |
| Server GC + DATAS | 动态自适应低延迟 GC，适合游戏服务器 |
| `delegate* unmanaged` | 与 C++ 函数指针直接互操作，零 marshaling |
| `UnmanagedCallersOnly` | C# 方法可直接被 C++ 以原生调用约定调用 |
| NativeAOT 成熟 | 未来可将服务端也 AOT 编译 |
| `FrozenDictionary` / `UnsafeAccessor` | 适合 AOT 场景的高性能基础设施 |

### 2.2 为什么深度使用 Source Generator

Unity IL2CPP（AOT 编译）的限制：

| IL2CPP 禁区 | 传统做法（IL2CPP 崩溃） | Source Generator 做法（安全） |
|-------------|----------------------|---------------------------|
| 动态创建类型 | `Activator.CreateInstance(type)` | 编译期生成工厂 `switch` |
| 序列化 | `PropertyInfo.GetValue()` 反射 | 编译期生成直接字段读写 |
| RPC 分发 | `MethodInfo.Invoke(target, args)` | 编译期生成 `switch(rpcId)` |
| 脏标记 | 运行时 Proxy / IL weaving | 编译期生成属性 setter 标记 |
| 事件绑定 | 反射扫描 `[EventHandler]` | 编译期生成 `RegisterAll()` |
| Native 互操作 | `Marshal.GetDelegateForFunctionPointer` | 编译期生成 `delegate* unmanaged` 包装 |

### 2.3 整体架构

```
┌──────────────────────────────────────────────────────────────┐
│                    编译期 (Source Generator)                   │
│  ┌────────────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐  │
│  │ Interop Gen    │ │Entity Gen│ │  RPC Gen │ │Event Gen │  │
│  │ Native函数绑定 │ │序列化    │ │存根/代理 │ │事件注册  │  │
│  │ 类型编组       │ │脏标记    │ │消息路由  │ │          │  │
│  │ LibraryImport  │ │工厂注册  │ │ID分配    │ │          │  │
│  └───────┬────────┘ └────┬─────┘ └────┬─────┘ └────┬─────┘  │
│          └───────────────┴────────────┴─────────────┘        │
│                          │                                    │
│                 生成的纯 C# 代码 (零反射)                     │
└──────────────────────────┼────────────────────────────────────┘
                           │
               ┌───────────┴───────────┐
               ▼                       ▼
        ┌──────────────┐      ┌──────────────┐
        │  服务端       │      │  Unity 客户端 │
        │  .NET 9 CLR │      │  IL2CPP (AOT)│
        │  JIT + 热重载 │      │  零反射限制   │
        └──────┬───────┘      └──────────────┘
               │
        ┌──────┴───────┐
        │  C++ Engine  │
        │  hostfxr嵌入  │
        └──────────────┘
```

## 3. 当前架构快照（Python 时代）

```
src/lib/
├── script/                    # 语言无关抽象（实际耦合 Python）
│   ├── script_object.hpp      # 纯虚基类 ScriptObject
│   ├── script_events.hpp/cpp  # ScriptEvents — 直接使用 PyObjectPtr
│   └── CMakeLists.txt         # 依赖 atlas_pyscript
│
└── pyscript/                  # Python 3 实现（16 个文件，待删除）
    ├── py_interpreter.hpp/cpp
    ├── py_object.hpp
    ├── py_type.hpp/cpp
    ├── py_module.hpp/cpp
    ├── py_convert.hpp
    ├── py_pickler.hpp/cpp
    ├── py_gil.hpp
    ├── py_error.hpp/cpp
    ├── atlas_module.hpp/cpp
    └── CMakeLists.txt
```

**核心问题**: `script_events.hpp` 第 2 行 `#include "pyscript/py_object.hpp"` 直接耦合 Python。

## 4. 目标架构（.NET 9 + Source Generator）

### 4.1 C++ 侧

```
src/lib/
├── script/                        # 语言无关抽象层（零 Python 依赖）
│   ├── script_engine.hpp          # [新建] ScriptEngine 抽象接口
│   ├── script_object.hpp          # [修改] 扩展调用/设置属性能力
│   ├── script_value.hpp/cpp       # [新建] 类型擦除值容器
│   ├── script_events.hpp/cpp      # [重写] 去 Python 化
│   └── CMakeLists.txt             # [修改] 不再依赖 pyscript
│
├── clrscript/                     # [新建] .NET CLR 实现
│   ├── clr_host.hpp/cpp           # CoreCLR 生命周期 (hostfxr)
│   ├── clr_host_windows.cpp       # Windows 平台适配
│   ├── clr_host_linux.cpp         # Linux 平台适配
│   ├── clr_object.hpp/cpp         # ClrObject (ScriptObject 实现)
│   ├── clr_marshal.hpp/cpp        # 类型编组
│   ├── clr_invoke.hpp/cpp         # C++ → C# 调用
│   ├── clr_native_api.hpp/cpp     # C# → C++ 导出函数（LibraryImport 目标）
│   ├── clr_export.hpp             # ATLAS_NATIVE_API 导出宏
│   ├── native_api_provider.hpp    # INativeApiProvider 接口（进程级适配）
│   ├── base_native_provider.hpp   # 基础 Provider（日志/时间/注册通用逻辑）
│   ├── clr_error.hpp/cpp          # 异常桥接
│   ├── clr_script_engine.hpp/cpp  # ClrScriptEngine (ScriptEngine 实现)
│   ├── clr_hot_reload.hpp/cpp     # 热重载管理
│   └── CMakeLists.txt
│
└── pyscript/                      # [删除] 整个目录
```

### 4.2 C# 侧

```
src/csharp/
├── Atlas.Generators.Interop/     # Source Generator: Native 互操作
├── Atlas.Generators.Def/         # Source Generator: 实体系统 (属性/序列化/RPC/工厂/类型注册)
├── Atlas.Generators.Events/      # Source Generator: 事件系统
├── Atlas.Shared/                 # 共享库 (netstandard2.1, IL2CPP 安全)
├── Atlas.Runtime/                # 服务端运行时 (net9.0)
└── Atlas.Runtime.Tests/          # xUnit 测试
```

## 5. 阶段总览

| 阶段 | 名称 | 预估周期 | 关键交付物 |
|------|------|----------|-----------|
| 0 | 清理 Python + 建立抽象层 | 1.5-2 周 | `ScriptEngine` / `ScriptValue` 接口; pyscript 删除 |
| 1 | .NET 9 运行时嵌入 | 2-3 周 | `ClrHost`; C++ 进程能调用 C# 方法 |
| 2 | C++ ↔ C# 互操作层 | 4-5 周 | `ClrMarshal` / `ClrObject`; `Atlas.Generators.Interop` |
| 3† | Atlas 引擎 C# 绑定 | 3-4 周 | `Atlas.Runtime`; `ClrScriptEngine`; 日志/时间/实体回调 |
| 4 | 共享程序集 + Source Generator | 3-4 周 | `Atlas.Shared`; Entity/Rpc/Events Generator |

> † ScriptPhase 3 启动前需先执行 ScriptPhase 4 的任务 4.1 (Atlas.Shared 项目) + 4.2 (Attribute 定义) + 4.3 (SpanWriter/SpanReader)，因为 `Atlas.Runtime` 引用 `Atlas.Shared`。
| 5 | 热重载机制 | 2-3 周 | `AssemblyLoadContext` 隔离; 文件监控; 状态迁移 |
| 6 | 测试与稳定化 | 3-4 周 | 完整测试矩阵; GC 调优; 性能基准; 文档 |
| **总计** | | **17-22 周** | |

## 6. 里程碑与验收标准

| 里程碑 | 验收标准 | 状态 |
|--------|----------|------|
| **M0: 抽象层就位** | `ScriptEvents` 不再依赖 `PyObjectPtr`; 项目编译不需要 Python SDK; 所有非 Python 测试通过 | ✅ 完成 |
| **M1: .NET 可加载** | C++ 进程能加载 CoreCLR 并调用 C# `[UnmanagedCallersOnly]` 方法返回正确结果 | ✅ 完成 |
| **M2: 双向互操作** | C++ 可调用 C# 方法, C# 可调用 C++ 导出函数; 支持基本类型 + string + byte[]; Interop Generator 生成可用代码 | ✅ 完成 — 116 个 C++ 测试通过；详见 [implementation_notes.md](scripting/implementation_notes.md) |
| **M3: 引擎可脚本化** | C# 脚本中可调用 `Atlas.Log.Info()`, `Atlas.Time.ServerTime`; Entity 生命周期回调工作 | 🟡 进行中 — `Atlas.Runtime`、`ClrScriptEngine`、生命周期回调与对应测试已落地，`atlas_module.cpp` 的全量 C# 对等能力仍在补齐 |
| **M4: 跨端共享** | 同一 `Atlas.Shared.dll` 在服务端 (.NET 9) 和 Unity IL2CPP 上编译运行; Source Generator 输出零反射代码 | 🟡 进行中 — `Atlas.Shared` 与 Entity/Rpc/Events Generator 已落地，Unity IL2CPP 全量验收仍未完成 |
| **M5: 热重载可用** | 修改 C# 脚本后无需重启服务端进程即可生效 | 🟡 进行中 — `ScriptLoadContext`、`HotReloadManager`、`ClrHotReload` 与文件监控已落地，自动编译/回滚链路仍需继续收口 |
| **M6: 生产就绪** | 全部测试通过; 10K 实体压测无内存泄漏; GC 暂停 < 5ms@p99 | 🟡 进行中 — C++/C# 测试矩阵已建立，但全量通过、压测与 GC 指标尚未完成正式验收 |

## 7. Python 删除清单

### 需要删除的文件（24 个）

| 类型 | 文件 | 数量 |
|------|------|------|
| 源码 | `src/lib/pyscript/` 整个目录 | 16 |
| 测试 | `tests/unit/test_py_interpreter.cpp` | 1 |
| 测试 | `tests/unit/test_py_object.cpp` | 1 |
| 测试 | `tests/unit/test_py_convert.cpp` | 1 |
| 测试 | `tests/unit/test_py_gil.cpp` | 1 |
| 测试 | `tests/unit/test_py_type.cpp` | 1 |
| 测试 | `tests/unit/test_py_module.cpp` | 1 |
| 测试 | `tests/unit/test_py_pickler.cpp` | 1 |
| 测试 | `tests/unit/test_atlas_module.cpp` | 1 |

### 需要修改的文件（5 个）

| 文件 | 变更 |
|------|------|
| `src/lib/CMakeLists.txt` | `pyscript` → `clrscript` |
| `src/lib/script/CMakeLists.txt` | 移除 `atlas_pyscript` 依赖 |
| `src/lib/script/script_events.hpp/cpp` | 移除 Python 耦合 |
| `cmake/AtlasFindPackages.cmake` | 移除 `find_package(Python3)`; 新增 .NET 发现 |
| `tests/unit/CMakeLists.txt` | 移除 8 个 `test_py_*` 条目 |

## 8. 详细阶段文档

各阶段的完整任务分解请参阅:

- [ScriptPhase 0: 清理 Python + 建立抽象层](script_phase0_cleanup_abstraction.md)
- [ScriptPhase 1: .NET 9 运行时嵌入](script_phase1_dotnet_host.md)
- [ScriptPhase 2: C++ ↔ C# 互操作层](script_phase2_interop_layer.md)
- [ScriptPhase 3: Atlas 引擎 C# 绑定](script_phase3_engine_bindings.md) ← 含序列化对齐
- [ScriptPhase 4: 共享程序集 + Source Generator](script_phase4_shared_generators.md) ← 含 Mailbox + EntityDef
- [ScriptPhase 5: 热重载机制](script_phase5_hot_reload.md)
- [ScriptPhase 6: 测试与稳定化](script_phase6_testing.md)

### 架构基础设施

- [NativeApi 架构基础设施](native_api_architecture.md) — **必读**: 共享库构建、INativeApiProvider 进程适配、线程安全（ScriptPhase 2–6 的基础）
- [实现笔记与经验教训](implementation_notes.md) — DLL TLS 隔离、CLR 双 Assembly 实例、Source Generator 链式限制等实现中发现的关键问题及解决方案

### 专题设计文档

- [Entity Mailbox 代理机制设计](entity_mailbox_design.md) — BigWorld 风格 `entity.client.SayHi()` 在 C# Source Generator 下的实现
- [C++ / C# 序列化格式对齐](serialization_alignment.md) — `SpanWriter`/`SpanReader` 与 `BinaryWriter`/`BinaryReader` 字节级兼容
- [实体类型注册机制](entity_type_registration.md) — 替代传统 `.def` XML，C# Attribute + Source Generator → C++ `EntityDefRegistry`
