using Atlas.Client;

namespace Atlas.Mvp.Client;

// Client-side shell for the per-space owner entity. Carries no visual
// state; SpaceData broadcasts ride a separate envelope path.
[Atlas.Entity.Entity("MvpSpace")]
public partial class MvpSpace : ClientEntity
{
}
