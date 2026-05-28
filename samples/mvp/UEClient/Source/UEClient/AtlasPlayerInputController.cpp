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
constexpr float kInputHz = 30.0f;
constexpr uint16 kInputDtMs = 33;

atlas::Vec3 UEUnitDirectionToAtlas(const FVector& Direction)
{
	const FVector Unit = Direction.IsNearlyZero() ? FVector::ForwardVector : Direction.GetSafeNormal();
	return atlas::Vec3{
		static_cast<float>(Unit.Y),
		static_cast<float>(Unit.Z),
		static_cast<float>(Unit.X)};
}
}  // namespace

UAtlasPlayerInputController::UAtlasPlayerInputController()
{
	PrimaryComponentTick.bCanEverTick = true;
}

bool UAtlasPlayerInputController::ResolveOwnerView()
{
	UWorld* World = GetWorld();
	if (World == nullptr) return false;
	UGameInstance* GI = World->GetGameInstance();
	if (GI == nullptr) return false;
	UAtlasSubsystem* Sub = GI->GetSubsystem<UAtlasSubsystem>();
	if (Sub == nullptr) return false;
	SubsystemPtr = Sub;

	if (!ViewPtr.IsValid())
	{
		AActor* Owner = GetOwner();
		ViewPtr = Owner != nullptr ? Owner->FindComponentByClass<UAtlasAvatarView>() : nullptr;
	}
	UAtlasAvatarView* View = ViewPtr.Get();
	atlas::mvp::Avatar* Avatar = View != nullptr ? View->GetEntity() : nullptr;
	if (Avatar == nullptr) return false;
	if (Avatar->Id() != Sub->GetPlayerEntityId()) return false;

	OwnerEntity = static_cast<FBpAvatarEntity*>(Avatar);
	return true;
}

void UAtlasPlayerInputController::InitializePredictor()
{
	if (OwnerEntity == nullptr) return;
	const atlas::Vec3& Pos = OwnerEntity->InitialServerPos();
	const atlas::Vec3& Dir = OwnerEntity->InitialServerDir();
	Predictor.Reset(Pos, Dir);
	InputAccum = 0.0f;

	if (AActor* Owner = GetOwner())
	{
		Owner->SetActorLocation(Predictor.RenderPositionUE());
		Owner->SetActorRotation(Predictor.RenderDirectionUE().Rotation());
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
		SubsystemPtr.Reset();
		bInitialized = false;
		InputAccum = 0.0f;
		return;
	}

	if (!bInitialized)
	{
		if (!OwnerEntity->HasInitialTransform()) return;
		InitializePredictor();
	}

	FBpAvatarEntity::FMovementAck Ack;
	if (OwnerEntity->ConsumeMovementAck(Ack))
	{
		float ErrorM = 0.0f;
		uint16 CorrectionFlags = 0;
		if (Predictor.ApplyMovementAck(
			    Ack.AckedInputSeq, Ack.ServerTick, Ack.State, ErrorM, CorrectionFlags) &&
		    SubsystemPtr.IsValid())
		{
			SubsystemPtr->SendMovementCorrectionReport(
				OwnerEntity->Id(), Ack.AckedInputSeq, Ack.ServerTick, ErrorM, CorrectionFlags);
		}
	}
	atlas::MovementCommandFrame CommandStart;
	if (OwnerEntity->ConsumeMovementCommandStart(CommandStart))
	{
		Predictor.ApplyMovementCommandStart(CommandStart);
	}
	FBpAvatarEntity::FMovementCommandEnd CommandEnd;
	if (OwnerEntity->ConsumeMovementCommandEnd(CommandEnd))
	{
		Predictor.ApplyMovementCommandEnd(
			CommandEnd.CommandId, CommandEnd.ServerTick, CommandEnd.Reason, CommandEnd.State);
	}
	Predictor.TickVisualOffset(DeltaTime);

	APlayerController* PC = UGameplayStatics::GetPlayerController(this, 0);
	if (PC == nullptr) return;

	float Forward = (PC->IsInputKeyDown(EKeys::W) ? 1.0f : 0.0f)
	              - (PC->IsInputKeyDown(EKeys::S) ? 1.0f : 0.0f);
	float Right = (PC->IsInputKeyDown(EKeys::D) ? 1.0f : 0.0f)
	            - (PC->IsInputKeyDown(EKeys::A) ? 1.0f : 0.0f);
	const FVector LocalInput(Forward, Right, 0.0f);
	if (LocalInput.SizeSquared() > 1.0f)
	{
		const FVector Clamped = LocalInput.GetSafeNormal();
		Forward = static_cast<float>(Clamped.X);
		Right = static_cast<float>(Clamped.Y);
	}

	const float CamYaw = PC->GetControlRotation().Yaw;
	AimDir = FRotator(0.0f, CamYaw, 0.0f).RotateVector(FVector::ForwardVector);
	if (AimDir.IsNearlyZero()) AimDir = FVector::ForwardVector;
	AimDir = AimDir.GetSafeNormal();

	InputAccum += FMath::Max(DeltaTime, 0.0f);
	bool bPushedInput = false;
	while (InputAccum >= 1.0f / kInputHz)
	{
		InputAccum -= 1.0f / kInputHz;
		if (!Predictor.AcceptsInput()) continue;
		bPushedInput |= Predictor.PushInput(
			Predictor.BuildInputFrame(Forward, Right, CamYaw, kInputDtMs));
	}

	if (bPushedInput && SubsystemPtr.IsValid() && OwnerEntity != nullptr)
	{
		const int32 Count = Predictor.CopyRecentFrames(SendFrames.data(), kSendFrameCapacity);
		if (Count > 0)
		{
			SubsystemPtr->SendMovementInput(OwnerEntity->Id(), SendFrames.data(), Count);
		}
	}

	if (AActor* Owner = GetOwner())
	{
		Owner->SetActorLocation(Predictor.RenderPositionUE());
		Owner->SetActorRotation(Predictor.RenderDirectionUE().Rotation());
	}

	if (PC->WasInputKeyJustPressed(EKeys::SpaceBar) && OwnerEntity != nullptr)
	{
		OwnerEntity->LaunchProjectile(UEUnitDirectionToAtlas(AimDir));
	}
	if (PC->WasInputKeyJustPressed(EKeys::LeftShift) && OwnerEntity != nullptr)
	{
		OwnerEntity->Dash(UEUnitDirectionToAtlas(AimDir));
	}
}

void UAtlasPlayerInputController::EndPlay(const EEndPlayReason::Type Reason)
{
	if (OwnerEntity != nullptr) OwnerEntity->SetOwnerInputActive(false);
	OwnerEntity = nullptr;
	SubsystemPtr.Reset();
	bInitialized = false;
	Super::EndPlay(Reason);
}
