#include "AtlasUEActorView.h"
#include "AtlasCoordinates.h"

void FAtlasUEActorView::OnTransformReplicated(const atlas::Vec3& Position,
                                              const atlas::Quat& /*Rotation*/)
{
	AActor* Pinned = Actor.Get();
	if (!Pinned) return;
	Pinned->SetActorLocation(AtlasToUE(Position));
}
