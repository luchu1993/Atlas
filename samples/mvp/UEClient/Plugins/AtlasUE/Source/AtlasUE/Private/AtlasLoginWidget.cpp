#include "AtlasLoginWidget.h"

#include "Engine/GameInstance.h"

#include "AtlasSubsystem.h"

UAtlasLoginWidget::UAtlasLoginWidget(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

void UAtlasLoginWidget::NativeConstruct()
{
	Super::NativeConstruct();

	BoundSubsystem = ResolveSubsystem();
	if (BoundSubsystem == nullptr) return;

	BoundSubsystem->OnNetStateChanged.AddDynamic(this, &UAtlasLoginWidget::HandleNetStateChanged);
	BoundSubsystem->OnLoginFinished.AddDynamic(this, &UAtlasLoginWidget::HandleLoginFinished);
	BoundSubsystem->OnReconnectScheduled.AddDynamic(this, &UAtlasLoginWidget::HandleReconnectScheduled);
	BoundSubsystem->OnReconnectExhausted.AddDynamic(this, &UAtlasLoginWidget::HandleReconnectExhausted);
}

void UAtlasLoginWidget::NativeDestruct()
{
	if (BoundSubsystem != nullptr)
	{
		BoundSubsystem->OnNetStateChanged.RemoveDynamic(this, &UAtlasLoginWidget::HandleNetStateChanged);
		BoundSubsystem->OnLoginFinished.RemoveDynamic(this, &UAtlasLoginWidget::HandleLoginFinished);
		BoundSubsystem->OnReconnectScheduled.RemoveDynamic(this, &UAtlasLoginWidget::HandleReconnectScheduled);
		BoundSubsystem->OnReconnectExhausted.RemoveDynamic(this, &UAtlasLoginWidget::HandleReconnectExhausted);
		BoundSubsystem = nullptr;
	}
	Super::NativeDestruct();
}

bool UAtlasLoginWidget::BeginLoginFromFields(const FString& Host, int32 Port,
                                             const FString& Username, const FString& PasswordHash)
{
	const FString HostTrim = Host.TrimStartAndEnd();
	const FString UserTrim = Username.TrimStartAndEnd();
	if (HostTrim.IsEmpty() || UserTrim.IsEmpty()) return false;

	UAtlasSubsystem* Sub = ResolveSubsystem();
	if (Sub == nullptr) return false;
	return Sub->BeginLogin(HostTrim, Port, UserTrim, PasswordHash);
}

void UAtlasLoginWidget::HandleNetStateChanged(EAtlasNetClientState NewState)
{
	OnNetStateChanged(NewState);
}

void UAtlasLoginWidget::HandleLoginFinished(int32 Status)
{
	OnLoginFinished(Status);
}

void UAtlasLoginWidget::HandleReconnectScheduled(int32 AttemptNumber, float NextRetrySec)
{
	OnReconnectScheduled(AttemptNumber, NextRetrySec);
}

void UAtlasLoginWidget::HandleReconnectExhausted()
{
	OnReconnectExhausted();
}

UAtlasSubsystem* UAtlasLoginWidget::ResolveSubsystem() const
{
	const UGameInstance* GI = GetGameInstance();
	return GI != nullptr ? GI->GetSubsystem<UAtlasSubsystem>() : nullptr;
}
