using System;
using System.Runtime.InteropServices;

namespace Atlas.Client;

/// <summary>
/// Client-side C# → C++ interop. Binds to functions exported from the
/// client executable's shared library (atlas_client or atlas_engine).
/// </summary>
internal static unsafe partial class ClientNativeApi
{
    // LibName is conditional so the same source can be repurposed when/if
    // a Unity-style host ever ships this assembly. Desktop CoreCLR (the
    // atlas_client.exe path) hostfxr-embeds the runtime and exposes C API
    // exports from atlas_engine.dll (src/lib/clrscript/). Unity (Mono /
    // IL2CPP) would P/Invoke against atlas_net_client.dll instead — see
    // docs/UNITY_NATIVE_DLL_DESIGN.md §6.1. Atlas.Client.Desktop currently
    // only targets the desktop path; the conditional keeps the option
    // open without requiring a source edit at pickup time.
#if UNITY_IOS && !UNITY_EDITOR
    private const string LibName = "__Internal";
#elif UNITY_5_3_OR_NEWER || ATLAS_UNITY
    private const string LibName = "atlas_net_client";
#else
    private const string LibName = "atlas_engine";
#endif

    // =========================================================================
    // Logging
    // =========================================================================

    [LibraryImport(LibName, EntryPoint = "AtlasLogMessage")]
    private static partial void LogMessageNative(int level, byte* msg, int len);

    public static void LogMessage(int level, ReadOnlySpan<byte> message)
    {
        fixed (byte* ptr = message)
            LogMessageNative(level, ptr, message.Length);
    }

    // =========================================================================
    // RPC dispatch — client only sends base and cell RPCs
    // =========================================================================

    [LibraryImport(LibName, EntryPoint = "AtlasSendBaseRpc")]
    private static partial void SendBaseRpcNative(uint entityId, uint rpcId, byte* payload, int len);

    public static void SendBaseRpc(uint entityId, uint rpcId, ReadOnlySpan<byte> payload)
    {
        fixed (byte* ptr = payload)
            SendBaseRpcNative(entityId, rpcId, ptr, payload.Length);
    }

    [LibraryImport(LibName, EntryPoint = "AtlasSendCellRpc")]
    private static partial void SendCellRpcNative(uint entityId, uint rpcId, byte* payload, int len);

    public static void SendCellRpc(uint entityId, uint rpcId, ReadOnlySpan<byte> payload)
    {
        fixed (byte* ptr = payload)
            SendCellRpcNative(entityId, rpcId, ptr, payload.Length);
    }

    // =========================================================================
    // Entity type registry
    // =========================================================================

    [LibraryImport(LibName, EntryPoint = "AtlasRegisterEntityType")]
    private static partial void RegisterEntityTypeNative(byte* data, int len);

    public static void RegisterEntityType(ReadOnlySpan<byte> data)
    {
        fixed (byte* ptr = data)
            RegisterEntityTypeNative(ptr, data.Length);
    }

    // =========================================================================
    // Callback registration
    // =========================================================================

    [LibraryImport(LibName, EntryPoint = "AtlasSetNativeCallbacks")]
    private static partial void SetNativeCallbacksNative(void* callbacks, int len);

    public static void SetNativeCallbacks(void* callbacks, int len)
    {
        SetNativeCallbacksNative(callbacks, len);
    }

    // =========================================================================
    // ABI version
    // =========================================================================

    [LibraryImport(LibName, EntryPoint = "AtlasGetAbiVersion")]
    public static partial uint GetAbiVersion();
}
