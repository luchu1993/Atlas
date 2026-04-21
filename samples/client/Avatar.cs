using System;
using Atlas.Client;
using Atlas.DataTypes;

namespace Atlas.ClientSample;

// Reference implementation for the Phase-B client-side callback contract.
// Every hook the generator / base class wires up is overridden here with a
// `Console.WriteLine` breadcrumb so an operator running atlas_client.exe can
// eyeball the order in which events fire on AoI enter, wire deltas, volatile
// position, and destroy.
//
// Handler shape reminders (all source: Atlas.Client.ClientEntity):
//   * OnInit           — factory → instance created, no state yet
//   * OnEnterWorld     — initial transform + snapshot applied (Phase B3)
//   * OnHpChanged(..)  — wire delta on `hp` property (Phase B1)
//   * OnPositionUpdated(newPos) — volatile channel (Phase B4)
//   * OnDestroy        — leave AoI / logout
[Atlas.Entity.Entity("Avatar")]
public partial class Avatar : ClientEntity
{
    // -------- Lifecycle (A / B3) --------------------------------------------
    //
    // The base declares these `protected internal virtual`. Overrides in a
    // downstream assembly must use bare `protected` (C# forbids widening the
    // `internal` half across assemblies).

    protected override void OnInit()
    {
        Console.WriteLine($"[Avatar:{EntityId}] OnInit — instance created, no server state yet");
    }

    protected override void OnEnterWorld()
    {
        Console.WriteLine(
            $"[Avatar:{EntityId}] OnEnterWorld — complete initial state ready "
            + $"(pos={FormatVec(Position)}, hp={Hp})");
    }

    protected override void OnDestroy()
    {
        Console.WriteLine($"[Avatar:{EntityId}] OnDestroy — left AoI or tore down");
    }

    // -------- Property-change callbacks (B1, fired from ApplyReplicatedDelta)
    //
    // These are `partial` — the generator emits `partial void` declarations
    // (implicit private access), the script supplies the body with the same
    // implicit-private accessibility. Per Phase B2 (BigWorld-aligned), these
    // fire ONLY on wire deltas, never on script-local writes to the setter.

    partial void OnHpChanged(int oldValue, int newValue)
    {
        Console.WriteLine(
            $"[Avatar:{EntityId}] OnHpChanged: {oldValue} → {newValue} "
            + $"({(newValue < oldValue ? "damage" : "heal")})");
    }

    partial void OnLevelChanged(int oldValue, int newValue)
    {
        // Own-client only: fires only for the local player's own avatar
        // (scope=own_client). Useful to see that audience filtering works.
        Console.WriteLine($"[Avatar:{EntityId}] OnLevelChanged: {oldValue} → {newValue}");
    }

    partial void OnModelIdChanged(int oldValue, int newValue)
    {
        // Other-clients only: fires for peers observed via AoI
        // (scope=other_clients), never on own entity.
        Console.WriteLine($"[Avatar:{EntityId}] OnModelIdChanged: {oldValue} → {newValue}");
    }

    partial void OnManaChanged(int oldValue, int newValue)
    {
        // cell_public_and_own: owner-only on the replicated side.
        Console.WriteLine($"[Avatar:{EntityId}] OnManaChanged: {oldValue} → {newValue}");
    }

    partial void OnSecretChanged(string oldValue, string newValue)
    {
        // base_and_client (reliable): demonstrates a string delta crossing
        // the reliable channel (0xF003).
        Console.WriteLine(
            $"[Avatar:{EntityId}] OnSecretChanged: \"{oldValue}\" → \"{newValue}\"");
    }

    // -------- Transform (B4) -------------------------------------------------

    protected override void OnPositionUpdated(Vector3 newPos)
    {
        // Fires on every volatile position update (kEntityPositionUpdate
        // envelope). Deliberately does NOT get mixed into the property
        // callback chain: position is high-frequency, callback-free on the
        // snapshot path, and semantically a transform rather than a property.
        Console.WriteLine($"[Avatar:{EntityId}] OnPositionUpdated: {FormatVec(newPos)}");
    }

    // -------- RPC receivers (pre-existing A) --------------------------------

    public partial void ShowDamage(int amount, uint attackerId)
    {
        Console.WriteLine(
            $"[Avatar:{EntityId}] ShowDamage RPC: amount={amount}, attackerId={attackerId}");
    }

    public partial void ShowHeal(int amount)
    {
        Console.WriteLine($"[Avatar:{EntityId}] ShowHeal RPC: amount={amount}");
    }

    private static string FormatVec(Vector3 v)
        => $"({v.X:F2}, {v.Y:F2}, {v.Z:F2})";
}
