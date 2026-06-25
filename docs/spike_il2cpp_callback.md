# IL2CPP Callback Pattern Decision

**Status:** Current decision record for Unity callback interop. Re-run the
probe before changing Unity runtime, target platform, or callback pattern.

**Current verdict:** ✅ **Pattern B** (`[MonoPInvokeCallback]` + delegate +
`Marshal.GetFunctionPointerForDelegate`) — adopt now.
**Forward path:** Migrate to Pattern A (`[UnmanagedCallersOnly]` +
function pointer) only after the target Unity runtime exposes compatible
function-pointer support and this probe passes on every target platform.
Migration is mechanical; see [Forward compatibility](#forward-compatibility).

## Result matrix

| Target | Pattern A (`[UnmanagedCallersOnly]`) | Pattern B (`[MonoPInvokeCallback]`) |
|---|---|---|
| Editor (Mono, Unity 2022 LTS) | ❌ | ✅ |
| Editor (Mono, current MVP Unity 6 `6000.0.43f1-lilith-2`) | ❌ | ✅ |
| Standalone Windows IL2CPP (same Unity 6 editor line) | ❌ | ✅ |
| Android arm64 IL2CPP | Re-run per target | Re-run per target |
| iOS arm64 IL2CPP | Re-run per target | Re-run per target |

**Why A fails today:** Unity 2022.3 LTS and the current MVP Unity 6 editor
ship an old Mono / .NET 4.x runtime (or IL2CPP transpiled from same), neither of which supports
`[UnmanagedCallersOnly]`. The attribute exists in newer .NET, but Unity's
embedded runtime doesn't recognize it, so the JIT/AOT chain throws at
attribute lookup or silently emits a non-callable function pointer. Treat
other Unity 6.x releases as new validation targets and rerun this probe before
changing the callback bridge.

**When A becomes available:** do not rely on a forecasted Unity version.
Adopt Pattern A only after the exact target Unity editor/player runtime
supports `[UnmanagedCallersOnly]` function pointers and this probe passes on
every target platform.

## Current integration contract

- `AtlasNetCallbackBridge` uses Pattern B: declare `delegate` types per
  callback shape, attribute the static handler with
  `[MonoPInvokeCallback(typeof(...))]`, hold a static delegate field to
  keep the GC from collecting it, register via
  `Marshal.GetFunctionPointerForDelegate`.
- NULL fields in `AtlasNetCallbacks` mean "use the DLL's internal noop".
  The DLL substitutes any NULL slot before storing the table; C# never
  sees sentinel symbols.
- P/Invoke declarations remain plain `DllImport` / `LibraryImport` shapes.

## Forward compatibility

When a target Unity release with compatible runtime support is adopted and we
want to flip to Pattern A:

1. **Re-run the probe** in `src/tools/il2cpp_probe/` against the target
   runtime; confirm Pattern A fires on all target platforms. Don't skip
   this — Unity has historically delayed runtime features past initial
   release.
2. **Add a build-time toggle** `ATLAS_CALLBACK_PATTERN_A` (Player
   Settings → Scripting Define Symbols).
3. **Inside `AtlasNetCallbackBridge`**, the two patterns are isomorphic:

   ```csharp
   // Pattern B (current)
   delegate void OnRpcDelegate(nint ctx, uint eid, uint rid,
                               byte* payload, int len);

   [MonoPInvokeCallback(typeof(OnRpcDelegate))]
   static void OnRpc(nint ctx, uint eid, uint rid, byte* payload, int len)
       => /* … handler body … */;

   static OnRpcDelegate s_onRpcKeepAlive = OnRpc;
   nint fnPtr = Marshal.GetFunctionPointerForDelegate(s_onRpcKeepAlive);

   // Pattern A (future runtime candidate)
   [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
   static void OnRpc(nint ctx, uint eid, uint rid, byte* payload, int len)
       => /* … same handler body … */;

   unsafe { nint fnPtr = (nint)(delegate* unmanaged[Cdecl]<...>)&OnRpc; }
   ```

   Migration is per-callback mechanical: swap the attribute, drop the
   delegate type and keep-alive field, change how `fnPtr` is obtained.
   **No DLL-side change required** — both produce the same
   `void(*)(...)` ABI from the C++ side's perspective.

4. **Drop the GC keep-alive fields** when fully on Pattern A. Until
   then, leave the `s_*KeepAlive` fields in place; Pattern A doesn't
   need them but tolerates them.

5. **Bump `ATLAS_NET_ABI_VERSION`** only if the change is observable
   from the C++ side, which it isn't — keep it stable.

## Probe artefacts

- `src/tools/il2cpp_probe/probe.cc` — minimal native library
- `src/tools/il2cpp_probe/CMakeLists.txt` — gated by
  `ATLAS_BUILD_IL2CPP_PROBE`
- `src/tools/il2cpp_probe/Unity/ProbeComponent.cs` — both patterns
  side-by-side; keep for re-validation when adopting a new Unity runtime
- `src/tools/il2cpp_probe/README.md` — operational steps

The probe stays in the tree as a regression check for the eventual
Pattern A migration. Don't delete it.
