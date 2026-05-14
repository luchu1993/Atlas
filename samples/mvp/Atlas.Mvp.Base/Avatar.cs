using Atlas.Entity;

namespace Atlas.Mvp.Base;

[Entity("Avatar")]
public partial class Avatar : BaseServerEntity
{
    private const float kAoIRadius = 50f;
    private const float kAoIHysteresis = 5f;

    protected override void OnInit(bool isReload)
    {
        if (isReload) return;
        SetAoIRadius(kAoIRadius, kAoIHysteresis);
    }
}
