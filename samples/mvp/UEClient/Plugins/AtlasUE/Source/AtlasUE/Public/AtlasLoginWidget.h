#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"

#include "AtlasNetClient.h"

#include "AtlasLoginWidget.generated.h"

class UAtlasSubsystem;

// C++ base for the login UMG widget. Subclass in BP for layout / styling
// and override the BlueprintImplementableEvent hooks below to drive the UI.
UCLASS(Abstract, Blueprintable)
class ATLASUE_API UAtlasLoginWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	UAtlasLoginWidget(const FObjectInitializer& ObjectInitializer);

	// Wires up subsystem delegates so the BP events below start firing.
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;

	// Trim + dispatch into UAtlasSubsystem::BeginLogin; returns false on empty
	// host / username or an out-of-range port.
	UFUNCTION(BlueprintCallable, Category="Atlas|Login")
	bool BeginLoginFromFields(const FString& Host, int32 Port,
	                          const FString& Username, const FString& PasswordHash);

	UFUNCTION(BlueprintImplementableEvent, Category="Atlas|Login")
	void OnNetStateChanged(EAtlasNetClientState NewState);

	// Status mirrors AtlasLoginStatus; 0 = success, others surface via
	// UAtlasSubsystem::GetLastNetErrorMessage() for human-readable detail.
	UFUNCTION(BlueprintImplementableEvent, Category="Atlas|Login")
	void OnLoginFinished(int32 Status);

	UFUNCTION(BlueprintImplementableEvent, Category="Atlas|Login")
	void OnReconnectScheduled(int32 AttemptNumber, float NextRetrySec);

	UFUNCTION(BlueprintImplementableEvent, Category="Atlas|Login")
	void OnReconnectExhausted();

private:
	UFUNCTION()
	void HandleNetStateChanged(EAtlasNetClientState NewState);

	UFUNCTION()
	void HandleLoginFinished(int32 Status);

	UFUNCTION()
	void HandleReconnectScheduled(int32 AttemptNumber, float NextRetrySec);

	UFUNCTION()
	void HandleReconnectExhausted();

	UAtlasSubsystem* ResolveSubsystem() const;

	UPROPERTY(Transient)
	UAtlasSubsystem* BoundSubsystem = nullptr;
};
