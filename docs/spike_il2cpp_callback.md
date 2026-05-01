# Phase 0 Spike: IL2CPP Callback Pattern Decision

**Date:** 2026-05-01
**Verdict:** ✅ **Pattern B** (`[MonoPInvokeCallback]` + delegate +
`Marshal.GetFunctionPointerForDelegate`) — adopt now.
**Forward path:** Migrate to Pattern A (`[UnmanagedCallersOnly]` +
function pointer) when Unity 6.6+ ships with embedded .NET 10. Migration
is mechanical; see [Forward compatibility](#forward-compatibility).

## Result matrix

| Target | Pattern A (`[UnmanagedCallersOnly]`) | Pattern B (`[MonoPInvokeCallback]`) |
|---|---|---|
| Editor (Mono, Unity 2022 LTS) | ❌ | ✅ |
| Editor (Mono, Unity 6.x ≤ 6.5) | ❌ | ✅ |
| Standalone Windows IL2CPP (Unity ≤ 6.5) | ❌ | ✅ |
| Android arm64 IL2CPP (Unity ≤ 6.5) | ❌ | ✅ |
| iOS arm64 IL2CPP (Unity ≤ 6.5) | ❌ | ✅ |

**Why A fails today:** Unity 2022 through 6.5 ships an old Mono / .NET 4.x
runtime (or IL2CPP transpiled from same), neither of which supports
`[UnmanagedCallersOnly]`. The attribute exists in newer .NET, but Unity's
embedded runtime doesn't recognize it, so the JIT/AOT chain throws at
attribute lookup or silently emits a non-callable function pointer. This
covers every Unity version in current use.

**When A becomes available:** Unity 6.6 (per Unity's published roadmap)
replaces the embedded Mono/.NET-4.x with .NET 10 across both Editor and
IL2CPP backends. .NET 10 supports `[UnmanagedCallersOnly]` natively, so
Pattern A should light up unchanged. We **must** re-run this probe at
6.6 RTM before flipping the codebase.

## Decision impact on the design doc

- §6.3 (`AtlasNetCallbackBridge`): rewrite for Pattern B — declare
  `delegate` types per callback shape, attribute the static handler
  with `[MonoPInvokeCallback(typeof(...))]`, hold a static delegate
  field to keep the GC from collecting it, register via
  `Marshal.GetFunctionPointerForDelegate`.
- §4.0.4 (sentinel pattern): simplify. The original design had C# fetch
  function pointers to DLL-exported `AtlasNetNoop*` symbols, which is
  awkward in Pattern B (you can't `&[LibraryImport]Method` to get a
  raw native pointer in old IL2CPP). Switch to: **NULL fields in the
  `AtlasNetCallbacks` struct mean "use the DLL's internal noop"**. The
  DLL substitutes any NULL slot with its own sentinel before storing
  the table. C# never sees the sentinel symbols.
- §4.0.4.1 (sentinel acquisition mode A vs B): drop entirely.
- §6.1 P/Invoke declarations: no change.
- §10 Phase 0: status updated to ✅ done; this doc replaces the
  decision-pending placeholder.

## Forward compatibility

When Unity 6.6+ ships and we want to flip to Pattern A:

1. **Re-run the probe** in `src/tools/il2cpp_probe/` against the new
   runtime; confirm Pattern A fires on all four targets. Don't skip
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

   // Pattern A (Unity 6.6+ planned)
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
  side-by-side; keep for re-validation at Unity 6.6
- `src/tools/il2cpp_probe/README.md` — operational steps

The probe stays in the tree as a regression check for the eventual
Pattern A migration. Don't delete it.
