using System;
using Atlas.Client;
using Atlas.Client.Native;
using Xunit;

namespace Atlas.Client.Tests;

// LoginClient sits on top of atlas_net_client.dll; constructing one requires
// the DLL on PATH. These tests only validate types / value-shapes that don't
// require a live native context — full handshake coverage lives in
// tests/integration/test_client_flow.
public sealed class LoginClientTests
{
    [Fact]
    public void LoginResult_StoresHostAndPort()
    {
        var r = new LoginResult("10.0.0.5", 20013);
        Assert.Equal("10.0.0.5", r.BaseAppHost);
        Assert.Equal((ushort)20013, r.BaseAppPort);
    }

    [Fact]
    public void AuthResult_StoresEntityIdAndTypeId()
    {
        var r = new AuthResult(0x01000003, 7);
        Assert.Equal(0x01000003u, r.EntityId);
        Assert.Equal((ushort)7, r.TypeId);
    }

    [Fact]
    public void LoginFailedException_KeepsStatusAndMessage()
    {
        var ex = new LoginFailedException(AtlasLoginStatus.InvalidCredentials, "bad password");
        Assert.Equal(AtlasLoginStatus.InvalidCredentials, ex.Status);
        Assert.Equal("bad password", ex.Message);
    }

    [Fact]
    public void AuthFailedException_KeepsMessage()
    {
        var ex = new AuthFailedException("not yet ready");
        Assert.Equal("not yet ready", ex.Message);
    }

    [Fact]
    public void AtlasClientType_ExposesConnectAsyncSignature()
    {
        // Smoke check that the high-level facade's ConnectAsync exists with
        // the expected shape — guards against accidental rename / ctor-only
        // refactors that would silently break Unity / desktop callers.
        var t = typeof(Atlas.Client.Desktop.AtlasClient);
        var method = t.GetMethod("ConnectAsync");
        Assert.NotNull(method);
        var ps = method!.GetParameters();
        Assert.Equal(5, ps.Length);
        Assert.Equal("loginAppHost", ps[0].Name);
        Assert.Equal("loginAppPort", ps[1].Name);
        Assert.Equal("username", ps[2].Name);
        Assert.Equal("passwordHash", ps[3].Name);
    }
}
