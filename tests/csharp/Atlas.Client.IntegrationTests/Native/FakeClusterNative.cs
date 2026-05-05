using System;
using System.Runtime.InteropServices;

namespace Atlas.Client.IntegrationTests.Native;

internal static class FakeClusterNative
{
    private const string LibName = "atlas_test_helpers";

    [DllImport(LibName)]
    public static extern IntPtr AtlasFakeClusterCreate();

    [DllImport(LibName)]
    public static extern void AtlasFakeClusterDestroy(IntPtr cluster);

    [DllImport(LibName)]
    public static extern int AtlasFakeClusterStart(IntPtr cluster);

    [DllImport(LibName)]
    public static extern ushort AtlasFakeClusterLoginAppPort(IntPtr cluster);

    [DllImport(LibName)]
    public static extern void AtlasFakeClusterSetLoginPolicy(IntPtr cluster, byte policy);

    [DllImport(LibName)]
    public static extern void AtlasFakeClusterSetAuthPolicy(IntPtr cluster, byte policy);

    [DllImport(LibName)]
    public static extern void AtlasFakeClusterPump(IntPtr cluster, int budgetMs);

    [DllImport(LibName)]
    public static extern int AtlasFakeClusterLoginRequestSeen(IntPtr cluster);

    [DllImport(LibName)]
    public static extern int AtlasFakeClusterAuthenticateRequestSeen(IntPtr cluster);

    [DllImport(LibName)]
    public static extern int AtlasFakeClusterRpcReceived(IntPtr cluster);

    [DllImport(LibName)]
    public static extern uint AtlasFakeClusterLastRpcId(IntPtr cluster);

    [DllImport(LibName)]
    public static extern int AtlasFakeClusterPushEntityEnter(IntPtr cluster, uint eid,
        ushort typeId, float px, float py, float pz, float dx, float dy, float dz,
        int onGround, double serverTime);

    [DllImport(LibName)]
    public static extern int AtlasFakeClusterPushEntityPositionUpdate(IntPtr cluster, uint eid,
        float px, float py, float pz, float dx, float dy, float dz,
        int onGround, double serverTime);
}

public enum FakeLoginPolicy : byte
{
    Accept = 0,
    RejectInvalidCreds = 1,
    RejectServerFull = 2,
    NeverReply = 3,
}

public enum FakeAuthPolicy : byte
{
    Accept = 0,
    Reject = 1,
    NeverReply = 2,
}
