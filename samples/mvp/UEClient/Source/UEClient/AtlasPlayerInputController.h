#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "UObject/WeakObjectPtr.h"

#include <array>

#include "AtlasCore/client_entity.h"
#include "net_client/client_api.h"
#include "OwnerMovementPredictor.h"

#include "AtlasPlayerInputController.generated.h"

class UAtlasAvatarView;
class UAtlasSubsystem;
class FBpAvatarEntity;

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
	static constexpr int32 kSendFrameCapacity = 3;

	void InitializePredictor();
	bool ResolveOwnerView();

	TWeakObjectPtr<UAtlasAvatarView> ViewPtr;
	TWeakObjectPtr<UAtlasSubsystem> SubsystemPtr;
	FBpAvatarEntity* OwnerEntity = nullptr;

	std::array<AtlasMovementInputFrame, kSendFrameCapacity> SendFrames{};
	FOwnerMovementPredictor Predictor;
	FVector AimDir = FVector::ForwardVector;
	float InputAccum = 0.0f;
	bool bInitialized = false;
};
