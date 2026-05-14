using Atlas.Client;

namespace Atlas.Mvp.Client;

// Stub required by the entity factory before SelectAvatar hands the proxy
// off to the player Avatar — no client-side state of its own.
[Atlas.Entity.Entity("Account")]
public partial class Account : ClientEntity
{
}
