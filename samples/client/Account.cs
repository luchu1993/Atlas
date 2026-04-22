using System;
using Atlas.Client;

namespace Atlas.ClientSample;

// Client-side Account. Registers the type with ClientEntityFactory (via
// the generator's [ModuleInitializer]) so CreateEntity(typeId=Account)
// — which atlas_client fires right after Authenticate — lands on this
// class and OnInit gets to run. From OnInit we hand control off to the
// cluster's stress-test avatar by invoking the exposed SelectAvatar base
// method; the generator's RpcStub emits a SendBaseRpc for it.
//
// A dedicated Account script is required because Phase B's generated
// ClientEntityFactory.Register call only fires for types that have a
// matching [Entity("Name")] partial class in the assembly. Without this
// file the client silently drops CreateEntity(Account) and the flow
// stops at authenticate.
[Atlas.Entity.Entity("Account")]
public partial class Account : ClientEntity
{
    // AvatarIndex 1 encodes space_id=1 by convention (see
    // samples/stress/Atlas.StressTest.Base/Account.cs::SelectAvatar).
    private const int kDefaultAvatarIndex = 1;

    protected override void OnInit()
    {
        ClientLog.Info(
            $"[Account:{EntityId}] OnInit — calling SelectAvatar({kDefaultAvatarIndex})");
        SelectAvatar(kDefaultAvatarIndex);
    }

    protected override void OnDestroy()
    {
        ClientLog.Info($"[Account:{EntityId}] OnDestroy");
    }

    // RequestAvatarList is declared in Account.def but the stress harness
    // doesn't use it. Generator still requires a partial-method body on
    // the client side (base method exposed=own_client → Send stub on
    // client). No-op.
}
