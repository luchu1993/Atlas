#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"

#include "AtlasSubsystem.h"

#include "AtlasHudWidget.generated.h"

USTRUCT(BlueprintType)
struct FAtlasHudFrameStats
{
	GENERATED_BODY()

	// Smoothed FPS, computed from the widget's own tick deltaTime.
	UPROPERTY(BlueprintReadOnly, Category="Atlas|Hud")
	float Fps = 0.f;

	UPROPERTY(BlueprintReadOnly, Category="Atlas|Hud")
	FAtlasNetStatsBp Net;

	UPROPERTY(BlueprintReadOnly, Category="Atlas|Hud")
	EAtlasNetClientState NetState = EAtlasNetClientState::Idle;

	// Player avatar entity id; 0 before SelectAvatar completes.
	UPROPERTY(BlueprintReadOnly, Category="Atlas|Hud")
	int32 PlayerEntityId = 0;

	// SpaceData NpcCount (space_data.def key id 1) for space 1; -1 when the
	// server hasn't broadcast it yet.
	UPROPERTY(BlueprintReadOnly, Category="Atlas|Hud")
	int32 NpcCount = -1;
};

// C++ base for the in-game HUD UMG widget. Subclass in BP, override
// OnHudRefresh to push the FrameStats into Text/Image widgets.
UCLASS(Abstract, Blueprintable)
class ATLASUE_API UAtlasHudWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	UAtlasHudWidget(const FObjectInitializer& ObjectInitializer);

	virtual void NativeTick(const FGeometry& Geometry, float DeltaTime) override;

	UFUNCTION(BlueprintImplementableEvent, Category="Atlas|Hud")
	void OnHudRefresh(const FAtlasHudFrameStats& Stats);

	// Lower than 0.05 makes the FPS readout jitter; raise for smoother stats.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Atlas|Hud",
		meta=(ClampMin="0.05", ClampMax="2.0"))
	float RefreshIntervalSec = 0.25f;

private:
	float TimeSinceLastRefresh = 0.f;
	float SmoothedFps = 0.f;
};
