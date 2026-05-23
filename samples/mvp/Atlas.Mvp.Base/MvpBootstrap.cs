using Atlas;
using Atlas.Space;

namespace Atlas.Mvp.Base;

internal sealed class MvpBootstrap : IAtlasAppInitializer
{
    public void OnAppInit() => SpaceMaster.Register(/*spaceId=*/1, "MvpSpace");
}
