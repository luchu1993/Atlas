#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "UObject/WeakObjectPtr.h"

#include "AtlasPlayerInputController.generated.h"

class UAtlasAvatarView;
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
	void InitializeLocalSim();
	bool ResolveOwnerView();

	TWeakObjectPtr<UAtlasAvatarView> ViewPtr;
	FBpAvatarEntity* OwnerEntity = nullptr;

	FVector LocalPos = FVector::ZeroVector;
	FVector LocalDir = FVector::ForwardVector;
	float ReportAccum = 0.0f;
	bool bInitialized = false;
};
