#include "UEClientGameMode.h"

#include "Engine/GameInstance.h"
#include "Logging/LogMacros.h"
#include "Misc/Guid.h"

#include "AtlasSubsystem.h"
#include "AvatarCapsule.h"
#include "AvatarEntityStub.h"

DEFINE_LOG_CATEGORY_STATIC(LogUEClient, Log, All);

AUEClientGameMode::AUEClientGameMode()
{
	PrimaryActorTick.bCanEverTick = true;
	AvatarActorClass = AAvatarCapsule::StaticClass();
}

void AUEClientGameMode::BeginPlay()
{
	Super::BeginPlay();

	UAtlasSubsystem* Sub = GetGameInstance()->GetSubsystem<UAtlasSubsystem>();
	if (!Sub)
	{
		UE_LOG(LogUEClient, Error, TEXT("UAtlasSubsystem unavailable; aborting login"));
		return;
	}

	Sub->RegisterEntityClass(
		static_cast<uint16>(AvatarTypeId),
		AvatarActorClass,
		[](atlas::EntityId Id, atlas::EntityTypeId Type) -> std::unique_ptr<atlas::ClientEntity> {
			return std::make_unique<FAvatarEntityStub>(Id, Type);
		});

	if (Username.IsEmpty())
	{
		Username = FString::Printf(TEXT("mvp_%s"),
			*FGuid::NewGuid().ToString(EGuidFormats::Short).Left(8));
	}

	UE_LOG(LogUEClient, Log, TEXT("Atlas login host=%s port=%d user=%s"),
		*LoginHost, LoginPort, *Username);
	Sub->BeginLogin(LoginHost, static_cast<uint16>(LoginPort), Username, PasswordHash);
}

void AUEClientGameMode::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	UAtlasSubsystem* Sub = GetGameInstance()->GetSubsystem<UAtlasSubsystem>();
	if (!Sub) return;

	if (!bAuthenticateRequested && Sub->GetNetState() == EAtlasNetClientState::LoginSucceeded)
	{
		UE_LOG(LogUEClient, Log, TEXT("Atlas login succeeded, authenticating"));
		Sub->BeginAuthenticate();
		bAuthenticateRequested = true;
	}
}
