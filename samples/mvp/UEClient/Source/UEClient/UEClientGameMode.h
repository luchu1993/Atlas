#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "Templates/SubclassOf.h"

#include "AtlasNetClient.h"

#include "UEClientGameMode.generated.h"

UCLASS()
class AUEClientGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	AUEClientGameMode();

	UPROPERTY(EditAnywhere, Category = "Atlas|Login")
	FString LoginHost = TEXT("127.0.0.1");

	UPROPERTY(EditAnywhere, Category = "Atlas|Login")
	int32 LoginPort = 20018;

	// Empty username triggers FGuid-derived `mvp_xxxxxxxx`.
	UPROPERTY(EditAnywhere, Category = "Atlas|Login")
	FString Username;

	UPROPERTY(EditAnywhere, Category = "Atlas|Login")
	FString PasswordHash = TEXT("mvp_hash");

	UPROPERTY(EditAnywhere, Category = "Atlas|Avatar")
	TSubclassOf<AActor> AvatarActorClass;

protected:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

private:
	bool bAuthenticateRequested = false;
	bool bSelectAvatarSent = false;
	// Any → LoggingIn transition resets the per-session flags above so the
	// post-reconnect Authenticate + SelectAvatar fires again.
	EAtlasNetClientState LastNetState = EAtlasNetClientState::Idle;
};
