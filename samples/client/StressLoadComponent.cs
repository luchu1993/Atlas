using System.Collections.Generic;
using Atlas.Client;
using Atlas.Diagnostics;

namespace Atlas.Components;

// Client-side user partial for StressLoadComponent. Codegen emits the
// class declaration + ApplyDelta + RPC partial in Atlas.Components; this
// file fills in the receive-side method bodies the dispatcher routes
// component RPCs into.
public sealed partial class StressLoadComponent
{
    public partial void OnAffixesUpdated(List<int> ids)
    {
        Log.Info(
            $"[StressLoadComponent:{Entity.EntityId}] OnAffixesUpdated count={ids.Count}");
    }
}
