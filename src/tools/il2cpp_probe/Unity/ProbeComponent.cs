// Phase 0 IL2CPP callback feasibility probe — Unity side.
//
// Drop this file (and a build of `atlas_il2cpp_probe` with the matching
// platform suffix) into a Unity 2022.3 LTS project under
// `Assets/Plugins/IL2CPPProbe/`. Attach `ProbeComponent` to any GameObject
// in a test scene and run the four target builds:
//   - Editor (Mono)              — baseline reference
//   - Standalone Windows IL2CPP  — desktop IL2CPP path
//   - Android arm64 IL2CPP       — primary Unity mobile target
//   - iOS arm64 (DllImport "__Internal") — static-link path
//
// The component fires both Pattern A and Pattern B in `Start()` and logs
// which one delivered the callback. The decision matrix in
// `docs/client/UNITY_NATIVE_DLL_DESIGN.md` §10 Phase 0 is driven entirely
// by which pattern produces "A fired 42" / "B fired 42" lines.

using System.Runtime.InteropServices;
using AOT;
using UnityEngine;

namespace Atlas.IL2CPPProbe
{
    public class ProbeComponent : MonoBehaviour
    {
#if UNITY_IOS && !UNITY_EDITOR
        private const string LibName = "__Internal";
#else
        private const string LibName = "atlas_il2cpp_probe";
#endif

        // ---- Native imports -------------------------------------------------

        [DllImport(LibName)]
        private static extern void probe_set_callback(System.IntPtr cb);

        [DllImport(LibName)]
        private static extern void probe_fire(int value);

        // ---- Pattern A: [UnmanagedCallersOnly] + function pointer ----------
        //
        // .NET 5+ native style. IL2CPP support depends on Unity version
        // (Unity 2022.3 documents support; the spike validates this).

        [System.Runtime.InteropServices.UnmanagedCallersOnly(
            CallConvs = new[] { typeof(System.Runtime.CompilerServices.CallConvCdecl) })]
        private static void OnProbeA(int value)
        {
            Debug.Log($"[ProbeComponent] Pattern A fired with {value}");
        }

        // ---- Pattern B: [MonoPInvokeCallback] + delegate -------------------
        //
        // IL2CPP-native style. AOT-compiles the delegate into a native
        // trampoline; widely shipped in Unity. Fallback if A fails.

        private delegate void ProbeDelegate(int value);

        [MonoPInvokeCallback(typeof(ProbeDelegate))]
        private static void OnProbeB(int value)
        {
            Debug.Log($"[ProbeComponent] Pattern B fired with {value}");
        }

        // Keep the delegate rooted so the GC doesn't collect it before the
        // native side calls it. Static field is sufficient for the spike.
        private static ProbeDelegate s_keepAlive;

        // ---- Probe driver ---------------------------------------------------

        private void Start()
        {
            Debug.Log($"[ProbeComponent] Platform={Application.platform} " +
                      $"Backend={(IsIL2CPP() ? "IL2CPP" : "Mono")}");

            // ---- Pattern A ------------------------------------------------
            try
            {
                unsafe
                {
                    delegate* unmanaged[Cdecl]<int, void> fnPtr = &OnProbeA;
                    probe_set_callback((System.IntPtr)fnPtr);
                }
                probe_fire(42);
            }
            catch (System.Exception ex)
            {
                Debug.LogError($"[ProbeComponent] Pattern A threw: {ex}");
            }

            // ---- Pattern B ------------------------------------------------
            try
            {
                s_keepAlive = OnProbeB;
                System.IntPtr fnPtr = Marshal.GetFunctionPointerForDelegate(s_keepAlive);
                probe_set_callback(fnPtr);
                probe_fire(42);
            }
            catch (System.Exception ex)
            {
                Debug.LogError($"[ProbeComponent] Pattern B threw: {ex}");
            }
        }

        private static bool IsIL2CPP()
        {
#if ENABLE_IL2CPP
            return true;
#else
            return false;
#endif
        }
    }
}
