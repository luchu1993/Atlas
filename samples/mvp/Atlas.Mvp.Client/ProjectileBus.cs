using System;
using Atlas.DataTypes;

namespace Atlas.Mvp.Client;

// Decouples Mvp.Client (netstandard2.1) from the Unity visual layer: the
// visual controller subscribes here so Mvp.Client carries no Unity references.
public static class ProjectileBus
{
    public static event Action<uint, uint, Vector3, Vector3>? Fired;
    public static event Action<uint, Vector3, uint>? Ended;

    internal static void RaiseFired(uint shotId, uint ownerId, Vector3 origin, Vector3 velocity) =>
        Fired?.Invoke(shotId, ownerId, origin, velocity);

    internal static void RaiseEnded(uint shotId, Vector3 endPos, uint hitTargetId) =>
        Ended?.Invoke(shotId, endPos, hitTargetId);
}
