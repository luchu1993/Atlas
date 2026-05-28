using System;
using System.Runtime.InteropServices;

namespace Atlas.Client.Native;

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
    DefMismatch         = 6,
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

// Layout pinned by tests/unit/test_net_client_abi_layout.cpp.
[StructLayout(LayoutKind.Sequential, Pack = 1)]
public struct AtlasNetCallbacks
{
    public IntPtr OnDisconnect;
    public IntPtr OnDeliver;
}

[StructLayout(LayoutKind.Sequential, Pack = 1)]
public struct AtlasMovementInputFrame
{
    public uint Seq;
    public uint InputTick;
    public sbyte MoveX;
    public sbyte MoveZ;
    public ushort ViewYaw;
    public sbyte ViewPitch;
    public ushort Buttons;
    public ushort ClientDtMs;
}

[StructLayout(LayoutKind.Sequential, Pack = 1)]
public struct AtlasMovementStateFrame
{
    public float PositionX;
    public float PositionY;
    public float PositionZ;
    public float VelocityX;
    public float VelocityY;
    public float VelocityZ;
    public float DirectionX;
    public float DirectionY;
    public float DirectionZ;
    public uint Flags;
    public uint LastProcessedInputSeq;
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
