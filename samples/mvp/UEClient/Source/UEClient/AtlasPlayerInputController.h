#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "UObject/WeakObjectPtr.h"

#include "AtlasPlayerInputController.generated.h"

class UAtlasAvatarView;
class FBpAvatarEntity;

// Client-authoritative WASD movement + Space-fire for the avatar this actor
// represents. Owner detection is per-tick: bound entity id == subsystem's
// PlayerEntityId. Peer avatars carry the component too but it idles for
// them, so the same AAvatarCapsule blueprint serves both player and ghost
// instances. Reports position to the cell at kReportHz Hz, mirroring
// `samples/mvp/UnityClient/Assets/Scripts/PlayerInputController.cs`.
UCLASS(ClassGroup=(Atlas), meta=(BlueprintSpawnableComponent))
class UAtlasPlayerInputController : public UActorComponent
{
	GENERATED_BODY()

public:
	UAtlasPlayerInputController();

	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
	                           FActorComponentTickFunction* ThisTickFunction) override;

protected:
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;

private:
	// Snaps local sim to the actor's current position. Called the first tick
	// the controller detects it owns the bound entity, so the player capsule
	// starts on the server's spawn pos instead of teleporting to origin.
	void InitializeLocalSim();

	// Per-tick: returns true and updates ViewPtr if owner-bound this tick.
	bool ResolveOwnerView();

	TWeakObjectPtr<UAtlasAvatarView> ViewPtr;
	FBpAvatarEntity* OwnerEntity = nullptr;

	FVector LocalPos = FVector::ZeroVector;
	FVector LocalDir = FVector::ForwardVector;
	float ReportAccum = 0.0f;
	bool bInitialized = false;
};
