namespace Atlas.Components;

// Marker base for client-side-only state (HUD widgets, prediction
// buffers, visual-effect schedulers). No protocol involvement and no
// dirty tracking — the engine never serialises these. Lifecycle still
// flows through ClientComponentBase so OnAttached / OnDetached / OnTick
// hooks fire from the same pump as ReplicatedComponent.
public abstract class ClientLocalComponent : ClientComponentBase
{
}
