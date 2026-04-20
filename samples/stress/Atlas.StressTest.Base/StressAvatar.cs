using Atlas.Entity;

namespace Atlas.StressTest.Base;

// StressAvatar has no base_methods in its def, so there is nothing to
// implement here. The class still has to exist in the base-side assembly
// because DefGenerator only emits partial-class scaffolding for entities
// with a matching [Entity] declaration; without this stub the emitter
// bails out (cf. DefGenerator.cs: "if (defs.IsDefaultOrEmpty ||
// users.IsDefaultOrEmpty) return;"). Base-side scripts never run Echo /
// ReportPos — those are cell methods, implemented in the .Cell assembly.
[Entity("StressAvatar")]
public partial class StressAvatar : ServerEntity
{
}
