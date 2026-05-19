#include "UEClientGameMode.h"

#include "Blueprint/UserWidget.h"
#include "Engine/GameInstance.h"
#include "Logging/LogMacros.h"
#include "Misc/Guid.h"

#include "AtlasAvatarView.h"
#include "AtlasCore/moving_client_entity.h"
#include "AtlasHudWidget.h"
#include "AtlasLoginWidget.h"
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
// Returns 0 if the entity isn't in the loaded ATDF; caller MUST skip
// registration or server spawns of that type land on no factory and are lost.
uint16 ResolveTypeId(AtlasEdrContext* Ctx, const char* Name)
{
	const uint16 TypeId = AtlasEdrEntityTypeId(AtlasEdrFindEntityByName(Ctx, Name));
	if (TypeId == 0)
	{
		UE_LOG(LogUEClient, Error,
			TEXT("entity '%hs' missing from ATDF; will not register factory — server "
			     "spawns of this type will be dropped"), Name);
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
	if (const uint16 AccountType = ResolveTypeId(Edr, "Account"))
	{
		Sub->RegisterEntityClass(AccountType, AActor::StaticClass(),
			[](atlas::EntityId Id, atlas::EntityTypeId Type) -> std::unique_ptr<atlas::ClientEntity> {
				return std::make_unique<atlas::mvp::Account>(Id, Type);
			});
	}

	// FBpAvatarEntity bridges scalar change hooks → UAtlasAvatarView delegates.
	if (const uint16 AvatarType = ResolveTypeId(Edr, "Avatar"))
	{
		auto AvatarFactory = [](atlas::EntityId Id, atlas::EntityTypeId Type)
			-> std::unique_ptr<atlas::ClientEntity> {
			return std::make_unique<FBpAvatarEntity>(Id, Type);
		};
		// PostBind captures the GameMode so the player avatar's view can be
		// cached for BP-side HUD lookups (HP / level / RPC delegates).
		TWeakObjectPtr<AUEClientGameMode> WeakSelf(this);
		auto AvatarPostBind = [WeakSelf](atlas::ClientEntity* E, AActor* Actor) {
			auto* Bp = static_cast<FBpAvatarEntity*>(E);
			auto* View = Actor->FindComponentByClass<UAtlasAvatarView>();
			if (View != nullptr) Bp->BindView(View);
			if (auto* Self = WeakSelf.Get())
			{
				UAtlasSubsystem* Sub2 = Self->GetGameInstance()->GetSubsystem<UAtlasSubsystem>();
				if (Sub2 && static_cast<atlas::EntityId>(Sub2->GetPlayerEntityId()) == E->Id())
				{
					Self->PlayerAvatarView = View;
				}
			}
		};
		Sub->RegisterEntityClass(AvatarType, AvatarActorClass, AvatarFactory, AvatarPostBind);
	}

	// NPCs reuse the capsule mesh + movement interpolation; no BP bridge yet.
	if (const uint16 NpcType = ResolveTypeId(Edr, "Npc"))
	{
		const auto MovingFactory = [](atlas::EntityId Id, atlas::EntityTypeId Type)
			-> std::unique_ptr<atlas::ClientEntity> {
			return std::make_unique<atlas::MovingClientEntity>(Id, Type);
		};
		Sub->RegisterEntityClass(NpcType, AvatarActorClass, MovingFactory);
	}

	if (Username.IsEmpty())
	{
		Username = FString::Printf(TEXT("mvp_%s"),
			*FGuid::NewGuid().ToString(EGuidFormats::Short).Left(8));
	}

	// LoginWidgetClass takes over the submit flow when set; the BP widget calls
	// BeginLoginFromFields from its own Login button. Falls back to the cached
	// auto-login below so test/headless setups don't need a widget asset.
	if (LoginWidgetClass != nullptr)
	{
		APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
		LoginWidget = PC != nullptr
			? CreateWidget<UAtlasLoginWidget>(PC, LoginWidgetClass)
			: CreateWidget<UAtlasLoginWidget>(GetGameInstance(), LoginWidgetClass);
		if (LoginWidget != nullptr)
		{
			LoginWidget->AddToViewport();
			if (PC) PC->SetInputMode(FInputModeUIOnly().SetWidgetToFocus(LoginWidget->TakeWidget()));
			UE_LOG(LogUEClient, Log, TEXT("Atlas login widget shown — waiting for user submit"));
			return;
		}
		UE_LOG(LogUEClient, Warning,
			TEXT("LoginWidgetClass set but CreateWidget failed; falling back to auto-login"));
	}

	UE_LOG(LogUEClient, Log, TEXT("Atlas login host=%s port=%d user=%s"),
		*LoginHost, LoginPort, *Username);
	Sub->BeginLogin(LoginHost, LoginPort, Username, PasswordHash);
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
		if (LoginWidget != nullptr)
		{
			LoginWidget->RemoveFromParent();
			LoginWidget = nullptr;
			if (APlayerController* PC = GetWorld()->GetFirstPlayerController())
			{
				PC->SetInputMode(FInputModeGameOnly());
			}
		}
		if (HudWidgetClass != nullptr && HudWidget == nullptr)
		{
			APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
			HudWidget = PC != nullptr
				? CreateWidget<UAtlasHudWidget>(PC, HudWidgetClass)
				: CreateWidget<UAtlasHudWidget>(GetGameInstance(), HudWidgetClass);
			if (HudWidget != nullptr) HudWidget->AddToViewport();
		}
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
