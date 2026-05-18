#include "AtlasPlayerInputController.h"

#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"

#include "AtlasAvatarView.h"
#include "AtlasCoordinates.h"
#include "AtlasSubsystem.h"
#include "BpAvatarEntity.h"
#include "gen/Avatar.gen.h"

namespace
{
constexpr float kMoveSpeedMPerSec = 5.0f;
constexpr float kReportHz = 20.0f;
constexpr float kReportIntervalSec = 1.0f / kReportHz;
}  // namespace

UAtlasPlayerInputController::UAtlasPlayerInputController()
{
	PrimaryComponentTick.bCanEverTick = true;
}

bool UAtlasPlayerInputController::ResolveOwnerView()
{
	const UWorld* World = GetWorld();
	if (World == nullptr) return false;
	const UGameInstance* GI = World->GetGameInstance();
	if (GI == nullptr) return false;
	const UAtlasSubsystem* Sub = GI->GetSubsystem<UAtlasSubsystem>();
	if (Sub == nullptr) return false;

	if (!ViewPtr.IsValid())
	{
		AActor* Owner = GetOwner();
		ViewPtr = Owner != nullptr ? Owner->FindComponentByClass<UAtlasAvatarView>() : nullptr;
	}
	UAtlasAvatarView* View = ViewPtr.Get();
	atlas::mvp::Avatar* Avatar = View != nullptr ? View->GetEntity() : nullptr;
	if (Avatar == nullptr) return false;
	if (Avatar->Id() != Sub->GetPlayerEntityId()) return false;

	// AvatarFactory in UEClientGameMode always creates FBpAvatarEntity;
	// static_cast spares RTTI on the tick hot path.
	OwnerEntity = static_cast<FBpAvatarEntity*>(Avatar);
	return true;
}

void UAtlasPlayerInputController::InitializeLocalSim()
{
	if (OwnerEntity == nullptr) return;
	LocalPos = AtlasToUE(OwnerEntity->InitialServerPos());
	const FVector ServerDirUE = AtlasToUE(OwnerEntity->InitialServerDir());
	LocalDir = ServerDirUE.IsNearlyZero() ? FVector::ForwardVector : ServerDirUE.GetSafeNormal();
	if (AActor* Owner = GetOwner())
	{
		Owner->SetActorLocation(LocalPos);
		Owner->SetActorRotation(LocalDir.Rotation());
	}
	OwnerEntity->SetOwnerInputActive(true);
	bInitialized = true;
}

void UAtlasPlayerInputController::TickComponent(float DeltaTime, ELevelTick TickType,
                                                FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	const bool Owned = ResolveOwnerView();
	if (!Owned)
	{
		if (bInitialized && OwnerEntity != nullptr) OwnerEntity->SetOwnerInputActive(false);
		OwnerEntity = nullptr;
		bInitialized = false;
		return;
	}

	if (!bInitialized)
	{
		// Defer until AvatarFilter has the server pos; otherwise we'd snap
		// to spawn-default and lock the filter out before the real arrives.
		if (!OwnerEntity->HasInitialTransform()) return;
		InitializeLocalSim();
	}

	APlayerController* PC = UGameplayStatics::GetPlayerController(this, 0);
	if (PC == nullptr) return;

	const float Forward = (PC->IsInputKeyDown(EKeys::W) ? 1.0f : 0.0f)
	                    - (PC->IsInputKeyDown(EKeys::S) ? 1.0f : 0.0f);
	const float Right   = (PC->IsInputKeyDown(EKeys::D) ? 1.0f : 0.0f)
	                    - (PC->IsInputKeyDown(EKeys::A) ? 1.0f : 0.0f);

	FVector Input(Forward, Right, 0.0f);
	const bool Moving = !Input.IsNearlyZero();
	if (Moving) Input.Normalize();

	const float CamYaw = PC->GetControlRotation().Yaw;
	const FRotator YawRot(0.0f, CamYaw, 0.0f);
	const FVector WorldInput = YawRot.RotateVector(Input);

	const float StepCm = kMoveSpeedMPerSec * 100.0f * DeltaTime;
	LocalPos += WorldInput * StepCm;
	if (Moving) LocalDir = WorldInput.GetSafeNormal();

	if (AActor* Owner = GetOwner())
	{
		Owner->SetActorLocation(LocalPos);
		Owner->SetActorRotation(LocalDir.Rotation());
	}

	ReportAccum += DeltaTime;
	if (ReportAccum >= kReportIntervalSec && OwnerEntity != nullptr)
	{
		ReportAccum = 0.0f;
		OwnerEntity->ReportPos(UEToAtlas(LocalPos), UEToAtlas(LocalDir));
	}

	if (PC->WasInputKeyJustPressed(EKeys::SpaceBar) && OwnerEntity != nullptr)
	{
		OwnerEntity->LaunchProjectile(UEToAtlas(LocalDir));
	}
}

void UAtlasPlayerInputController::EndPlay(const EEndPlayReason::Type Reason)
{
	if (OwnerEntity != nullptr) OwnerEntity->SetOwnerInputActive(false);
	OwnerEntity = nullptr;
	bInitialized = false;
	Super::EndPlay(Reason);
}
