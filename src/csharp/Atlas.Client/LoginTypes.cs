using System;
using Atlas.Client.Native;

namespace Atlas.Client;

public readonly struct LoginResult
{
    public string BaseAppHost { get; }
    public ushort BaseAppPort { get; }
    public LoginResult(string host, ushort port) { BaseAppHost = host; BaseAppPort = port; }
}

public readonly struct AuthResult
{
    public uint EntityId { get; }
    public ushort TypeId { get; }
    public AuthResult(uint entityId, ushort typeId) { EntityId = entityId; TypeId = typeId; }
}

public sealed class LoginFailedException : Exception
{
    public AtlasLoginStatus Status { get; }
    public LoginFailedException(AtlasLoginStatus status, string message) : base(message)
    {
        Status = status;
    }
}

public sealed class AuthFailedException : Exception
{
    public AuthFailedException(string message) : base(message) { }
}
