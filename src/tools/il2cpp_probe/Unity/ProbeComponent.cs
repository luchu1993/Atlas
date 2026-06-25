// Unity-side callback pattern probe; copy with atlas_il2cpp_probe into a test project.
// Results update docs/spike_il2cpp_callback.md.

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

        [DllImport(LibName)]
        private static extern void probe_set_callback(System.IntPtr cb);

        [DllImport(LibName)]
        private static extern void probe_fire(int value);

        // Pattern A: .NET 5+ function pointer style. Unity support depends on
        // the runtime/backend version and must be verified by this probe.
        [System.Runtime.InteropServices.UnmanagedCallersOnly(
            CallConvs = new[] { typeof(System.Runtime.CompilerServices.CallConvCdecl) })]
        private static void OnProbeA(int value)
        {
            Debug.Log($"[ProbeComponent] Pattern A fired with {value}");
        }

        // Pattern B: IL2CPP-native delegate trampoline style. This is the
        // current Atlas callback bridge pattern.
        private delegate void ProbeDelegate(int value);

        [MonoPInvokeCallback(typeof(ProbeDelegate))]
        private static void OnProbeB(int value)
        {
            Debug.Log($"[ProbeComponent] Pattern B fired with {value}");
        }

        // Static field keeps Pattern B's delegate rooted until native code fires.
        private static ProbeDelegate s_keepAlive;

        private void Start()
        {
            Debug.Log($"[ProbeComponent] Platform={Application.platform} " +
                      $"Backend={(IsIL2CPP() ? "IL2CPP" : "Mono")}");

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
