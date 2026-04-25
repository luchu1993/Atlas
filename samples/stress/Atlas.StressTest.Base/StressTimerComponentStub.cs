using Atlas.Entity.Components;

// StressTimerComponent is fully implemented in Atlas.StressTest.Cell —
// the Cell process owns OnTick. Base never instantiates it, but
// EntityComponentAccessorEmitter still emits a typed `Timer` accessor
// on the StressAvatar partial because the .def declares the slot. The
// accessor needs the type to resolve, so a hand-written empty stub
// stands in here.
namespace Atlas.Components;

public sealed class StressTimerComponent : ServerLocalComponent
{
}
