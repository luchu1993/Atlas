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

	// Account has no spatial state — a bare AActor satisfies the subsystem's
	// Spawn-then-attach flow without rendering anything.
	Sub->RegisterEntityClass(1, AActor::StaticClass());

	const auto AvatarFactory = [](atlas::EntityId Id, atlas::EntityTypeId Type)
		-> std::unique_ptr<atlas::ClientEntity> {
		return std::make_unique<FAvatarEntityStub>(Id, Type);
	};

	Sub->RegisterEntityClass(static_cast<uint16>(AvatarTypeId), AvatarActorClass, AvatarFactory);

	// NPCs reuse the Avatar filter + capsule (same transform shape).
	Sub->RegisterEntityClass(4, AvatarActorClass, AvatarFactory);

	if (Username.IsEmpty())
	{
		Username = FString::Printf(TEXT("mvp_%s"),
			*FGuid::NewGuid().ToString(EGuidFormats::Short).Left(8));
	}

	// Digest now installed by UAtlasSubsystem::Initialize from the ATDF that
	// AtlasUE module loaded at startup. GameMode just kicks off login.

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

	if (!bSelectAvatarSent && Sub->GetNetState() == EAtlasNetClientState::Running
		&& Sub->GetPlayerEntityId() != 0)
	{
		// Hand-rolled Account.Base.SelectAvatar(1); rpc_id from
		// samples/mvp/Atlas.Mvp.Base/obj/.../RpcIds.g.cs#Account_SelectAvatar.
		const uint32 SelectAvatarRpcId = 0xC00101;
		const int32 AvatarIndex = 1;
		TArray<uint8> Payload;
		Payload.SetNumUninitialized(sizeof(int32));
		FMemory::Memcpy(Payload.GetData(), &AvatarIndex, sizeof(int32));

		const bool ok = Sub->SendBaseRpc(Sub->GetPlayerEntityId(), SelectAvatarRpcId, Payload);
		UE_LOG(LogUEClient, Log, TEXT("Sent Account.SelectAvatar(%d) entity_id=%u ok=%d"),
			AvatarIndex, Sub->GetPlayerEntityId(), ok);
		bSelectAvatarSent = ok;
	}
}
