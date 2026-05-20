using System.Runtime.CompilerServices;
using Atlas;
using Atlas.Space;

namespace Atlas.Mvp.Base;

// Registers MvpSpace as the space-owner type so the primary cellapp auto-spawns
// it the moment AddCellToSpace lands — NPC scene is alive before any login.
internal static class MvpBootstrap
{
    [ModuleInitializer]
    internal static void Register() => AppEvents.AppInit += OnAppInit;

    private static void OnAppInit() => SpaceMaster.Register(/*spaceId=*/1, "MvpSpace");
}
