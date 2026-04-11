using System;

namespace Atlas.Protocol;

/// <summary>
/// Interface for objects that can receive RPC dispatches.
/// </summary>
public interface IRpcTarget
{
    uint EntityId { get; }
}
