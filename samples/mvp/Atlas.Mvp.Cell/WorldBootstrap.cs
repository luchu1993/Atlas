using System.Runtime.CompilerServices;
using Atlas.DataTypes;
using Atlas.Entity;

namespace Atlas.Mvp.Cell;

internal static class WorldBootstrap
{
    private const uint kSpaceId = 1;

    [ModuleInitializer]
    public static void Register() => AppEvents.AppInit += SpawnWorld;

    private static void SpawnWorld()
    {
        var space = EntityFactory.CreateLocalCell("MvpSpace", kSpaceId, Vector3.Zero,
                                                  Vector3.Forward, onGround: false);
        if (space == null) AppEvents.DeferAppInit();
    }
}
