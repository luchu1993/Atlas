#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "AvatarCapsule.generated.h"

class UStaticMeshComponent;
class UAtlasAvatarView;
class UAtlasPlayerInputController;

UCLASS()
class AAvatarCapsule : public AActor
{
	GENERATED_BODY()

public:
	AAvatarCapsule();

	UPROPERTY(VisibleAnywhere, Category = "Atlas")
	TObjectPtr<UAtlasAvatarView> AvatarView;

	UPROPERTY(VisibleAnywhere, Category = "Atlas")
	TObjectPtr<UAtlasPlayerInputController> InputController;

private:
	UPROPERTY(VisibleAnywhere, Category = "Atlas")
	TObjectPtr<UStaticMeshComponent> Mesh;
};
