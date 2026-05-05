using System;
using Atlas.Client.Native;

namespace Atlas.Tools.NetClientDemo
{
    // FFI roundtrip verifier for atlas_net_client.dll.
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
            const uint kBadMajor = 0x03000000u;
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
            public void OnDeliver(ushort msgId, ReadOnlySpan<byte> payload)
                => Console.WriteLine($"  on_deliver msg=0x{msgId:X4} payload={payload.Length}B");
        }
    }
}
