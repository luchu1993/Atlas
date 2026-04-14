using System;

namespace Atlas.Rpc;

[AttributeUsage(AttributeTargets.Method)]
public sealed class ClientRpcAttribute : Attribute
{
    public bool Reliable { get; set; } = true;
}

[AttributeUsage(AttributeTargets.Method)]
public sealed class CellRpcAttribute : Attribute
{
    public bool Reliable { get; set; } = true;
}

[AttributeUsage(AttributeTargets.Method)]
public sealed class BaseRpcAttribute : Attribute
{
    public bool Reliable { get; set; } = true;
}
