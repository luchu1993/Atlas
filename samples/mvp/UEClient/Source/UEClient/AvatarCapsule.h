#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "AvatarCapsule.generated.h"

class UStaticMeshComponent;

UCLASS()
class AAvatarCapsule : public AActor
{
	GENERATED_BODY()

public:
	AAvatarCapsule();

private:
	UPROPERTY(VisibleAnywhere, Category = "Atlas")
	TObjectPtr<UStaticMeshComponent> Mesh;
};
