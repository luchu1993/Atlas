#include "AtlasUEActorView.h"
#include "AtlasCoordinates.h"

FAtlasUEActorView::~FAtlasUEActorView()
{
	if (AActor* Pinned = Actor.Get())
	{
		Pinned->Destroy();
	}
}

void FAtlasUEActorView::OnTransformReplicated(const atlas::Vec3& Position,
                                              const atlas::Quat& /*Rotation*/)
{
	AActor* Pinned = Actor.Get();
	if (!Pinned) return;
	Pinned->SetActorLocation(AtlasToUE(Position));
}
