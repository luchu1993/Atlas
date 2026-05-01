using System;
using Atlas.Client.Native;

namespace Atlas.Tools.NetClientDemo
{
    // .NET 9 FFI roundtrip verifier for atlas_net_client.dll.
    internal static class Program
    {
        private static int Main(string[] args)
        {
            Console.WriteLine($"atlas_net_client ABI = 0x{AtlasNetNative.AtlasNetGetAbiVersion():X8}");

            VerifyAbiMismatchRejected();
            var ctx = VerifyCreateAndState();
            VerifyCallbackTableInstall(ctx);
            VerifyDisconnectIdempotent(ctx);

            AtlasNetNative.AtlasNetDestroy(ctx);
            Console.WriteLine("FFI roundtrip OK");
            return 0;
        }

        private static void VerifyAbiMismatchRejected()
        {
            const uint kBadMajor = 0x02000000u;
            var bad = AtlasNetNative.AtlasNetCreate(kBadMajor);
            if (bad != IntPtr.Zero)
                throw new InvalidOperationException("expected ABI mismatch to reject");
            Console.WriteLine("ABI mismatch correctly rejected");
        }

        private static IntPtr VerifyCreateAndState()
        {
            var ctx = AtlasNetNative.Create();
            var state = AtlasNetNative.AtlasNetGetState(ctx);
            if (state != AtlasNetState.Disconnected)
                throw new InvalidOperationException($"expected Disconnected, got {state}");
            Console.WriteLine($"Create OK, state={state}");
            return ctx;
        }

        private static void VerifyCallbackTableInstall(IntPtr ctx)
        {
            var sink = new TraceSink();
            AtlasNetCallbackBridge.Register(ctx, sink);
            Console.WriteLine("Callback table installed");
        }

        private static void VerifyDisconnectIdempotent(IntPtr ctx)
        {
            int rc1 = AtlasNetNative.AtlasNetDisconnect(ctx, AtlasDisconnectReason.User);
            int rc2 = AtlasNetNative.AtlasNetDisconnect(ctx, AtlasDisconnectReason.User);
            if (rc1 != AtlasNetReturnCode.Ok || rc2 != AtlasNetReturnCode.Ok)
                throw new InvalidOperationException($"expected idempotent disconnect, got {rc1}/{rc2}");
            Console.WriteLine($"Disconnect idempotent (rc={rc1},{rc2})");
        }

        private sealed class TraceSink : IAtlasNetEvents
        {
            public void OnDisconnect(int reason) => Console.WriteLine($"  on_disconnect reason={reason}");
            public void OnPlayerBaseCreate(uint eid, ushort tid, ReadOnlySpan<byte> p)
                => Console.WriteLine($"  on_player_base_create eid={eid} tid={tid} props={p.Length}B");
            public void OnPlayerCellCreate(uint sid, float px, float py, float pz,
                                           float dx, float dy, float dz, ReadOnlySpan<byte> p)
                => Console.WriteLine($"  on_player_cell_create space={sid} pos=({px},{py},{pz})");
            public void OnResetEntities() => Console.WriteLine("  on_reset_entities");
            public void OnEntityEnter(uint eid, ushort tid,
                                      float px, float py, float pz,
                                      float dx, float dy, float dz, ReadOnlySpan<byte> p)
                => Console.WriteLine($"  on_entity_enter eid={eid} tid={tid}");
            public void OnEntityLeave(uint eid) => Console.WriteLine($"  on_entity_leave eid={eid}");
            public void OnEntityPosition(uint eid,
                                         float px, float py, float pz,
                                         float dx, float dy, float dz, bool onGround)
                => Console.WriteLine($"  on_entity_position eid={eid} ground={onGround}");
            public void OnEntityProperty(uint eid, byte scope, ReadOnlySpan<byte> d)
                => Console.WriteLine($"  on_entity_property eid={eid} scope={scope} delta={d.Length}B");
            public void OnForcedPosition(uint eid,
                                         float px, float py, float pz,
                                         float dx, float dy, float dz)
                => Console.WriteLine($"  on_forced_position eid={eid}");
            public void OnRpc(uint eid, uint rid, ReadOnlySpan<byte> p)
                => Console.WriteLine($"  on_rpc eid={eid} rid={rid} payload={p.Length}B");
        }
    }
}
