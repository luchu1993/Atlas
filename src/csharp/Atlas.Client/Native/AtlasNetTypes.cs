using System;
using System.Runtime.InteropServices;

namespace Atlas.Client.Native
{
    public enum AtlasNetState
    {
        Disconnected    = 0,
        LoggingIn       = 1,
        LoginSucceeded  = 2,
        Authenticating  = 3,
        Connected       = 4,
    }

    public enum AtlasLoginStatus : byte
    {
        Success             = 0,
        InvalidCredentials  = 1,
        AlreadyLoggedIn     = 2,
        ServerFull          = 3,
        Timeout             = 4,
        NetworkError        = 5,
        InternalError       = 255,
    }

    public enum AtlasDisconnectReason
    {
        User     = 0,
        Logout   = 1,
        Internal = 2,
    }

    public static class AtlasNetReturnCode
    {
        public const int Ok       = 0;
        public const int ErrBusy  = -16;
        public const int ErrNomem = -12;
        public const int ErrInval = -22;
        public const int ErrNoconn = -107;
        public const int ErrAbi   = -1000;
    }

    // 10 fn-ptr fields, Pack=1, layout pinned by tests/unit/test_net_client_abi_layout.cpp.
    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    public struct AtlasNetCallbacks
    {
        public IntPtr OnDisconnect;
        public IntPtr OnPlayerBaseCreate;
        public IntPtr OnPlayerCellCreate;
        public IntPtr OnResetEntities;
        public IntPtr OnEntityEnter;
        public IntPtr OnEntityLeave;
        public IntPtr OnEntityPosition;
        public IntPtr OnEntityProperty;
        public IntPtr OnForcedPosition;
        public IntPtr OnRpc;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    public struct AtlasNetStats
    {
        public uint  RttMs;
        public uint  BytesSent;
        public uint  BytesRecv;
        public uint  PacketsLost;
        public uint  SendQueueSize;
        public float LossRate;
    }
}
