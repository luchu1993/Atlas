#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "Templates/SubclassOf.h"

#include "AtlasNetClient.h"

#include "UEClientGameMode.generated.h"

class UAtlasAvatarView;
class UAtlasHudWidget;
class UAtlasLoginWidget;

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

	// When set, BeginPlay shows this widget instead of auto-calling BeginLogin.
	// The widget's BeginLoginFromFields() drives the actual login submit.
	UPROPERTY(EditAnywhere, Category = "Atlas|Login")
	TSubclassOf<UAtlasLoginWidget> LoginWidgetClass;

	// Shown after Authenticated (when the login widget gets removed); leave
	// unset for headless / test runs to skip HUD construction entirely.
	UPROPERTY(EditAnywhere, Category = "Atlas|Hud")
	TSubclassOf<UAtlasHudWidget> HudWidgetClass;

	// HP / level / RPC delegates of the avatar currently bound to the player;
	// nullptr until SelectAvatar resolves on the wire and the spawned actor's
	// UAtlasAvatarView component finishes its Bind() callback.
	UFUNCTION(BlueprintPure, Category = "Atlas|Player")
	UAtlasAvatarView* GetPlayerAvatarView() const { return PlayerAvatarView; }

protected:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

private:
	bool bAuthenticateRequested = false;
	bool bSelectAvatarSent = false;
	// Any → LoggingIn transition resets the per-session flags above so the
	// post-reconnect Authenticate + SelectAvatar fires again.
	EAtlasNetClientState LastNetState = EAtlasNetClientState::Idle;

	UPROPERTY(Transient)
	UAtlasLoginWidget* LoginWidget = nullptr;

	UPROPERTY(Transient)
	UAtlasHudWidget* HudWidget = nullptr;

	UPROPERTY(Transient)
	UAtlasAvatarView* PlayerAvatarView = nullptr;
};
