using System;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using Atlas.Core;

namespace Atlas.Client;

// ============================================================================
// DesktopBootstrap — CoreCLR-host specific glue, invoked explicitly by the
// host app (atlas_client.exe) once the CLR is up.
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
//
// Invocation contract — client_app.cc (desktop host) is expected to:
//   1. Initialise the CLR (Atlas.ClrHost's Bootstrap runs, ErrorBridge /
//      GCHandleHelper registered).
//   2. Call DesktopBootstrap.Initialize() via ClrHost::GetMethodAs.
//   3. Only then LoadModule the user's script assembly — any generated
//      ModuleInitializer inside it (TypeRegistry etc.) can now safely
//      call through ClientHost.
// The call is explicit rather than a [ModuleInitializer] so library
// consumers never have hidden side-effects on load (CA2255). The
// corresponding C++ wiring currently shares a pre-existing blocker with
// ClrScriptEngine's Atlas.Runtime.Lifecycle lookup — see
// docs/PHASE_C_VALIDATION.md Known Limitations §1.
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
    /// <summary>
    /// Fill ClientHost delegate slots with P/Invoke handlers and register
    /// the native callback table with atlas_engine. Must be called exactly
    /// once per process, after CLR bootstrap and before the generated
    /// ModuleInitializer code in the user's script assembly runs.
    /// Idempotent — repeat calls overwrite the slots with the same values.
    /// <para/>
    /// Exposed as an <c>[UnmanagedCallersOnly]</c> entry point so C++ can
    /// resolve it via <c>ClrHost::GetMethodAs</c>. Returns 0 on success,
    /// -1 on failure (with the error message set through ErrorBridge).
    /// </summary>
    [UnmanagedCallersOnly]
    public static int Initialize()
    {
        try
        {
            ClientHost.SendBaseRpcHandler = ClientNativeApi.SendBaseRpc;
            ClientHost.SendCellRpcHandler = ClientNativeApi.SendCellRpc;
            ClientHost.RegisterEntityTypeHandler = ClientNativeApi.RegisterEntityType;

            RegisterNativeCallbacks();
            return 0;
        }
        catch (Exception ex)
        {
            ErrorBridge.SetError(ex);
            return -1;
        }
    }

    /// <summary>
    /// Load the user's script assembly from <paramref name="pathUtf8"/>
    /// (UTF-8 bytes). Triggers every <c>[ModuleInitializer]</c> in the
    /// assembly, which wires the generator-emitted RPC dispatcher,
    /// entity factory, and type registry into <see cref="ClientHost"/>.
    /// The host must have called <see cref="Initialize"/> first so the
    /// ClientHost slots are populated before any of those initializers
    /// run.
    /// </summary>
    [UnmanagedCallersOnly]
    public static int LoadUserAssembly(byte* pathUtf8, int pathLen)
    {
        try
        {
            var path = Encoding.UTF8.GetString(new ReadOnlySpan<byte>(pathUtf8, pathLen));
            Assembly.LoadFrom(path);
            return 0;
        }
        catch (Exception ex)
        {
            ErrorBridge.SetError(ex);
            return -1;
        }
    }

    /// <summary>
    /// Build the native callback table and hand it to atlas_engine. Split
    /// out so tests can exercise the wire path without reinstalling the
    /// ClientHost handler slots.
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
