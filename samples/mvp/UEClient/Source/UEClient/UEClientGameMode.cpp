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

	// NPCs share Avatar's filter pipeline for M0; entity_ids.xml: Npc=4.
	Sub->RegisterEntityClass(4, AvatarActorClass, AvatarFactory);

	if (Username.IsEmpty())
	{
		Username = FString::Printf(TEXT("mvp_%s"),
			*FGuid::NewGuid().ToString(EGuidFormats::Short).Left(8));
	}

	// Hand-pasted entity_defs SHA-256; copy from
	// samples/mvp/Atlas.Mvp.Client/obj/.../EntityDefDigest.g.cs after any .def change.
	static constexpr uint8 EntityDefDigest[32] = {
		0x5B, 0x9E, 0x06, 0xC5, 0x85, 0xF4, 0x6A, 0xDE,
		0x82, 0xD7, 0x56, 0xD5, 0x1C, 0x8F, 0xB2, 0x72,
		0xD2, 0xF8, 0xAB, 0xCE, 0x3E, 0x9F, 0x9F, 0xF6,
		0xD2, 0x61, 0x0A, 0x89, 0x20, 0x94, 0x13, 0x99,
	};
	Sub->SetEntityDefDigest(TArrayView<const uint8>(EntityDefDigest, 32));

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
