using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Atlas.Client;

// ============================================================================
// DesktopBootstrap — CoreCLR-host specific glue.
//
// * Installs ClientHost delegate slots so pure-managed Atlas.Client RPC-send
//   and entity-registry calls reach atlas_engine via P/Invoke.
// * Owns the [UnmanagedCallersOnly] bridge methods that expose managed
//   decoders to native callback tables.
// * Registers the callback table with atlas_engine via
//   ClientNativeApi.SetNativeCallbacks.
//
// Everything here requires .NET 5+ features (function pointers,
// [UnmanagedCallersOnly], [LibraryImport]) and consequently lives in
// Atlas.Client.Desktop. Unity builds do not include this assembly — the
// Unity package installs equivalent handlers via its own P/Invoke surface
// against atlas_net_client.dll.
// ============================================================================

[StructLayout(LayoutKind.Sequential, Pack = 1)]
internal unsafe struct ClientCallbackTable
{
    public nint DispatchRpc;
    public nint CreateEntity;
    public nint DestroyEntity;
    public nint DeliverFromServer;
}

public static unsafe class DesktopBootstrap
{
    [ModuleInitializer]
    internal static void Init()
    {
        // Pure-managed Atlas.Client types (ClientEntity / generated Type
        // registry module initializer) reach native code through these
        // slots. Wiring them in a ModuleInitializer means the first time
        // .NET loads Atlas.Client.Desktop (via ClientSample.dll's
        // ProjectReference), the slots get filled before any generated
        // ModuleInitializer in the game-layer assembly runs.
        ClientHost.SendBaseRpcHandler = ClientNativeApi.SendBaseRpc;
        ClientHost.SendCellRpcHandler = ClientNativeApi.SendCellRpc;
        ClientHost.RegisterEntityTypeHandler = ClientNativeApi.RegisterEntityType;

        RegisterNativeCallbacks();
    }

    /// <summary>
    /// Build the native callback table and hand it to atlas_engine.
    /// Exposed as public so test harnesses (or hosts that suppress module
    /// initializers) can call it manually.
    /// </summary>
    public static void RegisterNativeCallbacks()
    {
        ClientCallbackTable table;
        table.DispatchRpc = (nint)(delegate* unmanaged<uint, uint, byte*, int, void>)&DispatchRpc;
        table.CreateEntity = (nint)(delegate* unmanaged<uint, ushort, void>)&CreateEntity;
        table.DestroyEntity = (nint)(delegate* unmanaged<uint, void>)&DestroyEntity;
        table.DeliverFromServer =
            (nint)(delegate* unmanaged<ushort, byte*, int, void>)&DeliverFromServer;

        ClientNativeApi.SetNativeCallbacks(&table, sizeof(ClientCallbackTable));
    }

    // -------------------------------------------------------------------------
    // [UnmanagedCallersOnly] bridges — materialise native pointers/lengths
    // into ReadOnlySpan and delegate to the shared managed decoders in
    // Atlas.Client.ClientCallbacks.
    // -------------------------------------------------------------------------

    [UnmanagedCallersOnly]
    public static void DispatchRpc(uint entityId, uint rpcId, byte* payload, int len)
    {
        var span = len > 0 ? new ReadOnlySpan<byte>(payload, len) : ReadOnlySpan<byte>.Empty;
        ClientCallbacks.DispatchRpc(entityId, rpcId, span);
    }

    [UnmanagedCallersOnly]
    public static void CreateEntity(uint entityId, ushort typeId)
    {
        ClientCallbacks.CreateEntity(entityId, typeId);
    }

    [UnmanagedCallersOnly]
    public static void DestroyEntity(uint entityId)
    {
        ClientCallbacks.DestroyEntity(entityId);
    }

    [UnmanagedCallersOnly]
    public static void DeliverFromServer(ushort msgId, byte* payload, int len)
    {
        var span = len > 0 ? new ReadOnlySpan<byte>(payload, len) : ReadOnlySpan<byte>.Empty;
        ClientCallbacks.DeliverFromServer(msgId, span);
    }
}
