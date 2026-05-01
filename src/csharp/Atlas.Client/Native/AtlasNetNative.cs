using System;
using System.Runtime.InteropServices;

namespace Atlas.Client.Native
{
    // Stays netstandard2.1 + DllImport (no LibraryImport) so Unity's Mono /
    // IL2CPP can consume the same source. Lib name resolution: the runtime
    // appends platform extension (.dll / .so / .bundle / .dylib); on iOS
    // the static-linked path uses "__Internal".
    public static unsafe class AtlasNetNative
    {
        public const uint AbiVersion = 0x01000000u;

#if UNITY_IOS && !UNITY_EDITOR
        private const string LibName = "__Internal";
#else
        private const string LibName = "atlas_net_client";
#endif

        public delegate void LoginResultDelegate(IntPtr userData, byte status,
                                                 IntPtr baseappHostUtf8,
                                                 ushort baseappPort,
                                                 IntPtr errorMessageUtf8);

        public delegate void AuthResultDelegate(IntPtr userData, byte success,
                                                uint entityId, ushort typeId,
                                                IntPtr errorMessageUtf8);

        public delegate void LogDelegate(int level, IntPtr messageUtf8, int len);

        [DllImport(LibName)]
        public static extern uint AtlasNetGetAbiVersion();

        [DllImport(LibName)]
        public static extern IntPtr AtlasNetLastError(IntPtr ctx);

        [DllImport(LibName)]
        public static extern IntPtr AtlasNetGlobalLastError();

        [DllImport(LibName)]
        public static extern IntPtr AtlasNetCreate(uint expectedAbi);

        [DllImport(LibName)]
        public static extern void AtlasNetDestroy(IntPtr ctx);

        [DllImport(LibName)]
        public static extern int AtlasNetPoll(IntPtr ctx);

        [DllImport(LibName)]
        public static extern AtlasNetState AtlasNetGetState(IntPtr ctx);

        [DllImport(LibName, CharSet = CharSet.Ansi)]
        public static extern int AtlasNetLogin(IntPtr ctx,
                                               [MarshalAs(UnmanagedType.LPUTF8Str)] string loginappHost,
                                               ushort loginappPort,
                                               [MarshalAs(UnmanagedType.LPUTF8Str)] string username,
                                               [MarshalAs(UnmanagedType.LPUTF8Str)] string passwordHash,
                                               IntPtr callback, IntPtr userData);

        [DllImport(LibName)]
        public static extern int AtlasNetAuthenticate(IntPtr ctx, IntPtr callback, IntPtr userData);

        [DllImport(LibName)]
        public static extern int AtlasNetDisconnect(IntPtr ctx, AtlasDisconnectReason reason);

        [DllImport(LibName)]
        public static extern int AtlasNetSendBaseRpc(IntPtr ctx, uint entityId, uint rpcId,
                                                     byte* payload, int len);

        [DllImport(LibName)]
        public static extern int AtlasNetSendCellRpc(IntPtr ctx, uint entityId, uint rpcId,
                                                     byte* payload, int len);

        [DllImport(LibName)]
        public static extern int AtlasNetSetCallbacks(IntPtr ctx, ref AtlasNetCallbacks callbacks);

        [DllImport(LibName)]
        public static extern void AtlasNetSetLogHandler(IntPtr handler);

        [DllImport(LibName)]
        public static extern int AtlasNetGetStats(IntPtr ctx, out AtlasNetStats stats);

        // ABI-checked Create. Throws on mismatch with the DLL's reported reason.
        public static IntPtr Create()
        {
            IntPtr ctx = AtlasNetCreate(AbiVersion);
            if (ctx == IntPtr.Zero)
            {
                IntPtr errPtr = AtlasNetGlobalLastError();
                string err = errPtr == IntPtr.Zero ? "unknown" : Marshal.PtrToStringUTF8(errPtr) ?? "unknown";
                throw new InvalidOperationException(
                    $"atlas_net_client.AtlasNetCreate failed (abi=0x{AbiVersion:X8}): {err}");
            }
            return ctx;
        }
    }
}
