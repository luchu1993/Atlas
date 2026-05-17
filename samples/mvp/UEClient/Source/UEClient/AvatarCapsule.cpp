#include "AvatarCapsule.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "UObject/ConstructorHelpers.h"

#include "AtlasAvatarView.h"

AAvatarCapsule::AAvatarCapsule()
{
	PrimaryActorTick.bCanEverTick = false;

	Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	SetRootComponent(Mesh);
	AvatarView = CreateDefaultSubobject<UAtlasAvatarView>(TEXT("AvatarView"));

	// Engine cylinder is the closest no-asset placeholder until the project
	// supplies a real character mesh.
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CylinderMesh(
		TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	if (CylinderMesh.Succeeded())
	{
		Mesh->SetStaticMesh(CylinderMesh.Object);
	}
	Mesh->SetRelativeScale3D(FVector(0.6f, 0.6f, 1.0f));
}
