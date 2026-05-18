#include "UEClientGameMode.h"

#include "Engine/GameInstance.h"
#include "Logging/LogMacros.h"
#include "Misc/Guid.h"

#include "AtlasAvatarView.h"
#include "AtlasCore/moving_client_entity.h"
#include "AtlasSubsystem.h"
#include "AtlasUE.h"
#include "AvatarCapsule.h"
#include "BpAvatarEntity.h"
#include "entitydef/entitydef_api.h"

#include "gen/Account.gen.h"
#include "gen/Avatar.gen.h"
#include "gen/Npc.gen.h"

DEFINE_LOG_CATEGORY_STATIC(LogUEClient, Log, All);

AUEClientGameMode::AUEClientGameMode()
{
	PrimaryActorTick.bCanEverTick = true;
	AvatarActorClass = AAvatarCapsule::StaticClass();
}

namespace
{
uint16 ResolveTypeId(AtlasEdrContext* Ctx, const char* Name)
{
	const uint16 TypeId = AtlasEdrEntityTypeId(AtlasEdrFindEntityByName(Ctx, Name));
	if (TypeId == 0)
	{
		UE_LOG(LogUEClient, Warning, TEXT("entity '%hs' missing from registry; factory will idle"),
			Name);
	}
	return TypeId;
}
}  // namespace

void AUEClientGameMode::BeginPlay()
{
	Super::BeginPlay();

	UAtlasSubsystem* Sub = GetGameInstance()->GetSubsystem<UAtlasSubsystem>();
	if (!Sub)
	{
		UE_LOG(LogUEClient, Error, TEXT("UAtlasSubsystem unavailable; aborting login"));
		return;
	}

	AtlasEdrContext* Edr = FAtlasUEModule::GetEdrContext();
	if (Edr == nullptr)
	{
		UE_LOG(LogUEClient, Error, TEXT("AtlasEdr context unavailable; aborting login"));
		return;
	}

	// Account has no spatial state — bare AActor + typed Account so the
	// game-side login flow can call account->SelectAvatar() directly.
	Sub->RegisterEntityClass(ResolveTypeId(Edr, "Account"), AActor::StaticClass(),
		[](atlas::EntityId Id, atlas::EntityTypeId Type) -> std::unique_ptr<atlas::ClientEntity> {
			return std::make_unique<atlas::mvp::Account>(Id, Type);
		});

	// FBpAvatarEntity bridges scalar change hooks → UAtlasAvatarView delegates.
	auto AvatarFactory = [](atlas::EntityId Id, atlas::EntityTypeId Type)
		-> std::unique_ptr<atlas::ClientEntity> {
		return std::make_unique<FBpAvatarEntity>(Id, Type);
	};
	auto AvatarPostBind = [](atlas::ClientEntity* E, AActor* Actor) {
		auto* Bp = static_cast<FBpAvatarEntity*>(E);
		if (auto* View = Actor->FindComponentByClass<UAtlasAvatarView>())
		{
			Bp->BindView(View);
		}
	};
	Sub->RegisterEntityClass(ResolveTypeId(Edr, "Avatar"), AvatarActorClass,
		AvatarFactory, AvatarPostBind);

	// NPCs reuse the capsule mesh + movement interpolation; no BP bridge yet.
	const auto MovingFactory = [](atlas::EntityId Id, atlas::EntityTypeId Type)
		-> std::unique_ptr<atlas::ClientEntity> {
		return std::make_unique<atlas::MovingClientEntity>(Id, Type);
	};
	Sub->RegisterEntityClass(ResolveTypeId(Edr, "Npc"), AvatarActorClass, MovingFactory);

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

	const EAtlasNetClientState NetState = Sub->GetNetState();
	if (NetState == EAtlasNetClientState::LoggingIn && LastNetState != NetState)
	{
		bAuthenticateRequested = false;
		bSelectAvatarSent = false;
	}
	LastNetState = NetState;

	if (!bAuthenticateRequested && NetState == EAtlasNetClientState::LoginSucceeded)
	{
		UE_LOG(LogUEClient, Log, TEXT("Atlas login succeeded, authenticating"));
		Sub->BeginAuthenticate();
		bAuthenticateRequested = true;
	}

	if (!bSelectAvatarSent && NetState == EAtlasNetClientState::Running
		&& Sub->GetPlayerEntityId() != 0)
	{
		atlas::ClientEntity* E = Sub->GetEntityManager().Find(Sub->GetPlayerEntityId());
		auto* Account = (E != nullptr && E->TypeId() == atlas::mvp::Account::kTypeId)
			? static_cast<atlas::mvp::Account*>(E) : nullptr;
		if (Account == nullptr) return;
		Account->SelectAvatar(1);
		UE_LOG(LogUEClient, Log, TEXT("Sent Account.SelectAvatar(1) entity_id=%u"),
			Sub->GetPlayerEntityId());
		bSelectAvatarSent = true;
	}
}
