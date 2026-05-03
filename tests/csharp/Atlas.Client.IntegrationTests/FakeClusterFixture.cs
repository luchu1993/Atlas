using System;
using Atlas.Client.IntegrationTests.Native;

namespace Atlas.Client.IntegrationTests;

// Shared via xunit IClassFixture so cluster startup cost (RUDP bind) is
// paid once per test class.
public sealed class FakeClusterFixture : IDisposable
{
    private IntPtr _handle;

    public FakeClusterFixture()
    {
        _handle = FakeClusterNative.AtlasFakeClusterCreate();
        if (_handle == IntPtr.Zero)
            throw new InvalidOperationException("AtlasFakeClusterCreate returned null");
        if (FakeClusterNative.AtlasFakeClusterStart(_handle) == 0)
            throw new InvalidOperationException("AtlasFakeClusterStart failed");
    }

    public IntPtr Handle => _handle;

    public ushort LoginAppPort => FakeClusterNative.AtlasFakeClusterLoginAppPort(_handle);

    public void SetLoginPolicy(FakeLoginPolicy policy)
        => FakeClusterNative.AtlasFakeClusterSetLoginPolicy(_handle, (byte)policy);

    public void SetAuthPolicy(FakeAuthPolicy policy)
        => FakeClusterNative.AtlasFakeClusterSetAuthPolicy(_handle, (byte)policy);

    public void Pump(int budgetMs)
        => FakeClusterNative.AtlasFakeClusterPump(_handle, budgetMs);

    public bool LoginRequestSeen
        => FakeClusterNative.AtlasFakeClusterLoginRequestSeen(_handle) != 0;
    public bool AuthenticateRequestSeen
        => FakeClusterNative.AtlasFakeClusterAuthenticateRequestSeen(_handle) != 0;
    public bool RpcReceived
        => FakeClusterNative.AtlasFakeClusterRpcReceived(_handle) != 0;
    public uint LastRpcId
        => FakeClusterNative.AtlasFakeClusterLastRpcId(_handle);

    public void Dispose()
    {
        if (_handle == IntPtr.Zero) return;
        FakeClusterNative.AtlasFakeClusterDestroy(_handle);
        _handle = IntPtr.Zero;
    }
}
