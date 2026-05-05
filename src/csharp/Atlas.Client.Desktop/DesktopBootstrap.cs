using System;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using Atlas.Client.Desktop;
using Atlas.Core;
using Atlas.Diagnostics;

namespace Atlas.Client;

// CoreCLR-host glue invoked explicitly by atlas_client.exe; Unity uses its
// own P/Invoke surface against atlas_net_client.dll instead.

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
    // Idempotent. Must run after CLR bootstrap and before the user assembly's
    // ModuleInitializer fires (which calls into ClientHost for digest /
    // entity-type registration).
    [UnmanagedCallersOnly]
    public static int Initialize()
    {
        try
        {
            // Force AutoFlush on managed stdout / stderr. .NET's default
            // Console writers leave a ~2 KB buffer in front of the handle
            // when stdout is a redirected pipe, so harnesses that poll
            // child stdout (world_stress --script-clients) or kill the
            // child before graceful exit see the first few hundred
            // Console.WriteLine lines vanish. Line-rate flushing is
            // harmless — desktop-client log volume never saturates it.
            Console.SetOut(new System.IO.StreamWriter(Console.OpenStandardOutput())
            {
                AutoFlush = true,
            });
            Console.SetError(new System.IO.StreamWriter(Console.OpenStandardError())
            {
                AutoFlush = true,
            });

            Log.SetBackend(new ConsoleLogBackend());

            ClientHost.SendBaseRpcHandler = ClientNativeApi.SendBaseRpc;
            ClientHost.SendCellRpcHandler = ClientNativeApi.SendCellRpc;
            ClientHost.RegisterEntityTypeHandler = ClientNativeApi.RegisterEntityType;
            ClientHost.RegisterStructHandler = ClientNativeApi.RegisterStruct;
            ClientHost.SetEntityDefDigestHandler = ClientNativeApi.SetEntityDefDigest;
            ClientHost.ReportEventSeqGapHandler = ClientNativeApi.ReportEventSeqGap;

            RegisterNativeCallbacks();
            return 0;
        }
        catch (Exception ex)
        {
            ErrorBridge.SetError(ex);
            return -1;
        }
    }

    // Triggers every [ModuleInitializer] in the user assembly, which wires
    // the generator-emitted dispatcher / factory / digest into ClientHost.
    [UnmanagedCallersOnly]
    public static int LoadUserAssembly(byte* pathUtf8, int pathLen)
    {
        try
        {
            var path = Encoding.UTF8.GetString(new ReadOnlySpan<byte>(pathUtf8, pathLen));

            // Load the user assembly into the SAME AssemblyLoadContext that
            // already holds Atlas.Client (which is the ALC this very method
            // runs in). hostfxr's load_assembly_and_get_function_pointer
            // puts each distinct script assembly into its own "isolated"
            // ALC, so using AssemblyLoadContext.Default or Assembly.LoadFrom
            // here would give us two distinct Atlas.Client.ClientEntityFactory
            // types: the generator's ModuleInitializer would register entity
            // creators into the user-assembly's copy while the
            // [UnmanagedCallersOnly] bridges in this file read from the
            // Atlas.Client.Desktop copy, and every CreateEntity callback
            // would come back null.
            var hostAlc = System.Runtime.Loader.AssemblyLoadContext.GetLoadContext(
                typeof(DesktopBootstrap).Assembly);
            if (hostAlc == null)
            {
                ErrorBridge.SetError(new InvalidOperationException(
                    "DesktopBootstrap: unable to resolve host AssemblyLoadContext"));
                return -1;
            }
            var assembly = hostAlc.LoadFromAssemblyPath(path);

            // The runtime defers [ModuleInitializer] execution until code
            // in the module is first accessed. atlas_client never touches
            // user-assembly types from managed code — all wiring happens
            // through C++ callbacks — so we force the module ctor to run
            // now so the generator-emitted registrations fire.
            foreach (var module in assembly.Modules)
            {
                System.Runtime.CompilerServices.RuntimeHelpers.RunModuleConstructor(
                    module.ModuleHandle);
            }
            return 0;
        }
        catch (Exception ex)
        {
            ErrorBridge.SetError(ex);
            return -1;
        }
    }

    // Split from Initialize so tests can exercise the wire path without
    // reinstalling the ClientHost handler slots.
    public static void RegisterNativeCallbacks()
    {
        ClientCallbackTable table;
        table.DispatchRpc =
            (nint)(delegate* unmanaged<uint, uint, byte*, int, ulong, void>)&DispatchRpc;
        table.CreateEntity = (nint)(delegate* unmanaged<uint, ushort, void>)&CreateEntity;
        table.DestroyEntity = (nint)(delegate* unmanaged<uint, void>)&DestroyEntity;
        table.DeliverFromServer =
            (nint)(delegate* unmanaged<ushort, byte*, int, void>)&DeliverFromServer;

        ClientNativeApi.SetNativeCallbacks(&table, sizeof(ClientCallbackTable));
    }

    // [UnmanagedCallersOnly] bridges to Atlas.Client.ClientCallbacks.

    [UnmanagedCallersOnly]
    public static void DispatchRpc(uint entityId, uint rpcId, byte* payload, int len,
                                   ulong traceId)
    {
        var span = len > 0 ? new ReadOnlySpan<byte>(payload, len) : ReadOnlySpan<byte>.Empty;
        ClientCallbacks.DispatchRpc(entityId, rpcId, traceId, span);
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
