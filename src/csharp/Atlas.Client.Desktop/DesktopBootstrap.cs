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
    // Idempotent; runs after CLR bootstrap and before user ModuleInitializers
    // call into ClientHost for digest and entity registration.
    [UnmanagedCallersOnly]
    public static int Initialize()
    {
        try
        {
            // Redirected stdout otherwise keeps a small managed buffer that
            // hides early harness output when child clients exit abruptly.
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
            ClientCallbacks.DefaultSession.SendBaseRpcHandler = ClientNativeApi.SendBaseRpc;
            ClientCallbacks.DefaultSession.SendCellRpcHandler = ClientNativeApi.SendCellRpc;
            ClientCallbacks.DefaultSession.ReportEventSeqGapHandler =
                ClientNativeApi.ReportEventSeqGap;

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

            // Load into the host ALC so generated registrations and native
            // bridges share the same Atlas.Client type identity.
            var hostAlc = System.Runtime.Loader.AssemblyLoadContext.GetLoadContext(
                typeof(DesktopBootstrap).Assembly);
            if (hostAlc == null)
            {
                ErrorBridge.SetError(new InvalidOperationException(
                    "DesktopBootstrap: unable to resolve host AssemblyLoadContext"));
                return -1;
            }
            var assembly = hostAlc.LoadFromAssemblyPath(path);

            // atlas_client reaches user code through C++ callbacks, so force
            // module initializers before the first native create callback.
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
        ClientCallbacks.DefaultSession.DispatchRpc(entityId, rpcId, traceId, span);
    }

    [UnmanagedCallersOnly]
    public static void CreateEntity(uint entityId, ushort typeId)
    {
        ClientCallbacks.DefaultSession.CreateEntity(entityId, typeId);
    }

    [UnmanagedCallersOnly]
    public static void DestroyEntity(uint entityId)
    {
        ClientCallbacks.DefaultSession.DestroyEntity(entityId);
    }

    [UnmanagedCallersOnly]
    public static void DeliverFromServer(ushort msgId, byte* payload, int len)
    {
        var span = len > 0 ? new ReadOnlySpan<byte>(payload, len) : ReadOnlySpan<byte>.Empty;
        ClientCallbacks.DefaultSession.DeliverFromServer(msgId, span);
    }
}
