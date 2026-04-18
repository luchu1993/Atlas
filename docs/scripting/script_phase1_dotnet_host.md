# ScriptPhase 1: .NET 9 运行时嵌入

> 预估周期: 2–3 周 | 前置依赖: ScriptPhase 0 完成 | **状态: ✅ 完成**

---

## 目标

1. 在 C++ 进程中嵌入 .NET 9 CoreCLR 运行时（通过 `hostfxr`）。
2. 实现 C++ → C# 的基本调用：获取 `[UnmanagedCallersOnly]` 方法的函数指针并调用。
3. 配置 Server GC、分层编译等运行时参数。
4. 建立 CMake 中 .NET SDK 发现与 C# 项目编译流程。
5. 跨平台支持：Windows + Linux。

## 验收标准 (M1)

- [x] C++ 进程能加载 CoreCLR 并成功初始化。
- [x] C++ 可调用 C# `[UnmanagedCallersOnly]` 静态方法并获取正确返回值。
- [x] `runtimeconfig.json` 配置 Server GC、DATAS、分层编译。
- [x] CMake 能发现 .NET SDK 并自动编译 C# 测试项目。
- [x] Windows 和 Linux 上均可运行。
- [x] 关闭时 CoreCLR 正常卸载，无泄漏。

## 实际落地（2026-04-18 核对）

| 产出 | 当前文件 | 说明 |
|------|---------|------|
| `ClrHost` 核心实现 | `src/lib/clrscript/clr_host.h` + `.cc` | `Initialize / Finalize / GetMethod` + 类型安全模板 `GetMethodAs<FuncPtr>` |
| 平台适配 | `src/lib/clrscript/clr_host_windows.cc` + `clr_host_linux.cc` | 各自经 `get_hostfxr_path` + `DynamicLibrary` 加载并解析符号 |
| Runtime 配置 | `runtime/atlas_server.runtimeconfig.json` (+ `.in` 模板) | `Server GC`/`Concurrent`/`RetainVM`/`DynamicAdaptationMode=1`/`TieredCompilation` |
| CMake 集成 | `cmake/FindDotNet.cmake` + `cmake/AtlasDotNetBuild.cmake` + `cmake/Dependencies.cmake` | 发现 `dotnet` + `hostfxr.lib`；`atlas_add_csharp_project()` 助手 |
| 冒烟测试 | `tests/csharp/Atlas.SmokeTest/` + `tests/unit/test_clr_host.cpp` | `Ping / Add / StringLength` 验证初始化、参数传递、`unsafe` 指针互操作 |

## 关键设计决策

1. **公共头不包含 `hostfxr.h` / `nethost.h`**：`clr_host.h` 将已解析的 hostfxr 入口缓存为 `void*`，仅在实现文件中 `reinterpret_cast` 为类型化签名。这样下游模块不需要 .NET SDK 头文件，编译隔离更干净。
2. **`DynamicLibrary` 必须为成员**：`hostfxr_lib_` 是 `std::optional<DynamicLibrary>`，保证 hostfxr 共享库在 `ClrHost` 存活期间一直被加载；否则缓存的函数指针会在 `LoadHostfxr()` 返回时立即失效。
3. **字符串编码**：Windows 上 `char_t = wchar_t`，`GetMethod` 使用 `MultiByteToWideChar` 做 UTF-8 → UTF-16 转换；Linux 上 `char_t = char`，直通。禁止使用 `std::wstring(sv.begin(), sv.end())` 之类的逐字符拷贝。
4. **`[UnmanagedCallersOnly]`**：C++ → C# 方向统一通过它，零 marshaling 开销，调用约定等同 C 函数。

## hostfxr 调用链参考

```
get_hostfxr_path()
  → DynamicLibrary::load(hostfxr_path)
  → hostfxr_initialize_for_runtime_config(runtimeconfig.json)
  → hostfxr_get_runtime_delegate(hdt_load_assembly_and_get_function_pointer)
  → load_assembly_and_get_function_pointer(asm, type, method, UNMANAGEDCALLERSONLY_METHOD)
  → [调用] fn_ptr(args...)
  → hostfxr_close(ctx)
```
