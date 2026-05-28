#include "OwnerMovementPredictor.h"

#include "AtlasCoordinates.h"
#include "MovementCommandCurves.h"
#include "MovementCommandMath.h"

namespace
{
using atlas::mvp::movement_math::AllowsTurnInput;
using atlas::mvp::movement_math::Dot;
using atlas::mvp::movement_math::IsFiniteVec3;
using atlas::mvp::movement_math::IsMovementCommandValid;
using atlas::mvp::movement_math::LengthSq;
using atlas::mvp::movement_math::Lerp;
using atlas::mvp::movement_math::SampleMovementCommandPosition;
using atlas::mvp::movement_math::Scale;
using atlas::mvp::movement_math::StateDirection;
using atlas::mvp::movement_math::StatePosition;
using atlas::mvp::movement_math::Subtract;
using atlas::mvp::movement_math::TryMovementCommandDirection;

constexpr uint32 kGroundedFlag = 1u;
constexpr float kSlowDecayCmPerSec = 300.0f;
constexpr float kFastDecayCmPerSec = 1200.0f;
constexpr float kTwoPi = 6.2831853071795864769f;

atlas::Vec3 DirectionFromInputYaw(const AtlasMovementInputFrame& Input)
{
	const float Yaw = (static_cast<float>(Input.view_yaw) / 65535.0f) * kTwoPi;
	return atlas::Vec3{FMath::Sin(Yaw), 0.0f, FMath::Cos(Yaw)};
}

uint32 ClampDeltaMs(float DeltaTime)
{
	return atlas::mvp::movement_math::ClampDeltaMs(static_cast<double>(DeltaTime));
}

FVector DirectionToUE(const atlas::Vec3& Direction)
{
	const FVector UE = AtlasToUE(Direction);
	return UE.IsNearlyZero() ? FVector::ForwardVector : UE.GetSafeNormal();
}

int8 QuantizeAxis(float Value)
{
	return static_cast<int8>(FMath::RoundToInt(FMath::Clamp(Value, -1.0f, 1.0f) * 127.0f));
}

uint16 QuantizeYaw(float Degrees)
{
	float Normalized = FMath::Fmod(Degrees, 360.0f);
	if (Normalized < 0.0f) Normalized += 360.0f;
	return static_cast<uint16>(FMath::RoundToInt((Normalized / 360.0f) * 65535.0f));
}
}

void FOwnerMovementPredictor::Reset(const atlas::Vec3& Position,
                                    const atlas::Vec3& Direction)
{
	PredictedState = AtlasMovementStateFrame{};
	PredictedState.position_x = Position.x;
	PredictedState.position_y = Position.y;
	PredictedState.position_z = Position.z;
	PredictedState.direction_x = Direction.x;
	PredictedState.direction_y = Direction.y;
	PredictedState.direction_z = Direction.z;
	if (AtlasToUE(Direction).IsNearlyZero())
	{
		PredictedState.direction_z = 1.0f;
	}
	PredictedState.flags = kGroundedFlag;
	HistoryStart = 0;
	HistoryCount = 0;
	LastAckSeq = 0;
	LastAckServerTick = 0;
	LastCommandStartServerTick = 0;
	LastCommandStartId = 0;
	LastCommandStartPriority = 0;
	NextInputSeq = 1;
	InputTick = 1;
	bHasLastAckSeq = false;
	bHasLastCommandStart = false;
	ActiveCommand = atlas::MovementCommandFrame{};
	ActiveCommandElapsedMs = 0;
	bHasOpenCommandStart = false;
	bHasActiveCommand = false;
	bHasCommandTurnInput = false;
	LastCommandEndReason = atlas::MovementCommandEndReason::kCompleted;
	VisualOffset = FVector::ZeroVector;
	VisualOffsetDecayCmPerSec = 0.0f;
}

AtlasMovementInputFrame FOwnerMovementPredictor::BuildInputFrame(
	float Forward, float Right, float CamYaw, uint16 InputDtMs)
{
	AtlasMovementInputFrame Frame{};
	Frame.seq = NextInputSeq++;
	Frame.input_tick = InputTick++;
	Frame.move_x = QuantizeAxis(Right);
	Frame.move_z = QuantizeAxis(Forward);
	Frame.view_yaw = QuantizeYaw(CamYaw);
	Frame.view_pitch = 0;
	Frame.buttons = 0;
	Frame.client_dt_ms = InputDtMs;
	return Frame;
}

bool FOwnerMovementPredictor::PushInput(const AtlasMovementInputFrame& Input)
{
	if (!ApplyInputForPrediction(Input, Input.input_tick)) return false;
	StoreInput(Input);
	return true;
}

bool FOwnerMovementPredictor::AcceptsInput() const
{
	return !bHasOpenCommandStart || AllowsTurnInput(ActiveCommand);
}

bool FOwnerMovementPredictor::ApplyInputForPrediction(
	const AtlasMovementInputFrame& Input, uint32 ServerTick)
{
	if (!bHasOpenCommandStart) return PredictStep(Input, ServerTick);
	if (!AllowsTurnInput(ActiveCommand)) return false;
	ApplyCommandTurnInput(Input);
	return true;
}

bool FOwnerMovementPredictor::PredictStep(const AtlasMovementInputFrame& Input,
                                          uint32 ServerTick)
{
	AtlasMovementStateFrame Next{};
	const int32 Result = AtlasNetMovementPredictStep(&PredictedState, &Input, ServerTick, &Next);
	if (Result != ATLAS_NET_OK) return false;
	PredictedState = Next;
	return true;
}

void FOwnerMovementPredictor::StoreInput(const AtlasMovementInputFrame& Input)
{
	if (HistoryCount == kHistoryCapacity)
	{
		HistoryStart = (HistoryStart + 1) % kHistoryCapacity;
		--HistoryCount;
	}
	InputHistory[(HistoryStart + HistoryCount) % kHistoryCapacity] = Input;
	++HistoryCount;
}

void FOwnerMovementPredictor::DropAcked(uint32 AckedInputSeq)
{
	while (HistoryCount > 0 && !IsInputSeqNewer(HistoryAt(0).seq, AckedInputSeq))
	{
		HistoryStart = (HistoryStart + 1) % kHistoryCapacity;
		--HistoryCount;
	}
}

bool FOwnerMovementPredictor::IsInputSeqNewer(uint32 Seq, uint32 Previous)
{
	return atlas::mvp::movement_math::IsInputSeqNewer(Seq, Previous);
}

bool FOwnerMovementPredictor::IsMovementAckNewer(uint32 AckedInputSeq, uint32 ServerTick,
	uint32 LastAckSeq, uint32 LastServerTick)
{
	return atlas::mvp::movement_math::IsMovementAckNewer(AckedInputSeq, ServerTick,
		LastAckSeq, LastServerTick);
}

uint32 FOwnerMovementPredictor::SeedNextInputSeqFromAck(uint32 CurrentNextInputSeq,
                                                        uint32 AckedInputSeq)
{
	const uint32 LastQueuedSeq = CurrentNextInputSeq - 1;
	return IsInputSeqNewer(AckedInputSeq, LastQueuedSeq) ?
		AckedInputSeq + 1 : CurrentNextInputSeq;
}

void FOwnerMovementPredictor::ReplayPending()
{
	for (int32 Index = 0; Index < HistoryCount; ++Index)
	{
		const AtlasMovementInputFrame& Input = HistoryAt(Index);
		if (!ApplyInputForPrediction(Input, Input.input_tick)) break;
	}
}

int32 FOwnerMovementPredictor::CopyRecentFrames(
	AtlasMovementInputFrame* OutFrames, int32 Capacity) const
{
	if (OutFrames == nullptr || Capacity <= 0) return 0;
	const int32 Count = FMath::Min(Capacity, HistoryCount);
	const int32 First = HistoryCount - Count;
	for (int32 Index = 0; Index < Count; ++Index)
	{
		OutFrames[Index] = HistoryAt(First + Index);
	}
	return Count;
}

bool FOwnerMovementPredictor::ApplyMovementAck(
	uint32 AckedInputSeq, uint32 ServerTick, const AtlasMovementStateFrame& State,
	float& OutErrorM, uint16& OutCorrectionFlags)
{
	OutErrorM = 0.0f;
	OutCorrectionFlags = 0;
	if (IsBehindLastCommandStart(ServerTick)) return false;
	if (bHasLastAckSeq &&
	    !IsMovementAckNewer(AckedInputSeq, ServerTick, LastAckSeq, LastAckServerTick))
	{
		return false;
	}

	const FVector RenderBefore = RenderPositionUE();
	LastAckSeq = AckedInputSeq;
	LastAckServerTick = ServerTick;
	bHasLastAckSeq = true;
	NextInputSeq = SeedNextInputSeqFromAck(NextInputSeq, AckedInputSeq);
	PredictedState = State;
	AlignActiveCommandToState(StatePosition(PredictedState));
	DropAcked(AckedInputSeq);
	ReplayPending();

	const FVector Corrected = AtlasToUE(StatePosition(PredictedState));
	const float ErrorCm = static_cast<float>(FVector::Dist(RenderBefore, Corrected));
	OutErrorM = ErrorCm / 100.0f;
	OutCorrectionFlags = AtlasNetMovementCorrectionFlag(OutErrorM);
	ApplyVisualCorrection(RenderBefore);
	return true;
}

bool FOwnerMovementPredictor::ApplyMovementCommandStart(
	const atlas::MovementCommandFrame& Command)
{
	if (!IsMovementCommandValid(Command)) return false;
	if (IsCommandStartStale(Command)) return false;
	const FVector RenderBefore = RenderPositionUE();
	ActiveCommand = Command;
	ActiveCommandElapsedMs = Command.elapsed_ms;
	LastCommandStartServerTick = Command.server_tick;
	LastCommandStartId = Command.command_id;
	LastCommandStartPriority = Command.priority;
	bHasLastCommandStart = true;
	bHasOpenCommandStart = true;
	bHasActiveCommand = true;
	bHasCommandTurnInput = false;
	HistoryStart = 0;
	HistoryCount = 0;
	ApplyCommandSample(0);
	ApplyVisualCorrection(RenderBefore);
	return true;
}

bool FOwnerMovementPredictor::ApplyMovementCommandEnd(
	uint32 CommandId, uint32 ServerTick, atlas::MovementCommandEndReason Reason,
	const AtlasMovementStateFrame& State)
{
	if (CommandId == 0) return false;
	if (IsBehindLastCommandStart(ServerTick)) return false;
	if (atlas::mvp::movement_math::IsAckStale(State.last_processed_input_seq, ServerTick,
	                                           LastAckSeq, LastAckServerTick, bHasLastAckSeq))
	{
		return false;
	}
	if (ActiveCommand.command_id != 0 && ActiveCommand.command_id != CommandId) return false;
	const FVector RenderBefore = RenderPositionUE();
	ActiveCommand = atlas::MovementCommandFrame{};
	ActiveCommandElapsedMs = 0;
	bHasOpenCommandStart = false;
	bHasActiveCommand = false;
	bHasCommandTurnInput = false;
	LastCommandEndReason = Reason;
	LastAckSeq = State.last_processed_input_seq;
	LastAckServerTick = ServerTick;
	bHasLastAckSeq = true;
	NextInputSeq = SeedNextInputSeqFromAck(NextInputSeq, State.last_processed_input_seq);
	PredictedState = State;
	DropAcked(State.last_processed_input_seq);
	ReplayPending();
	ApplyVisualCorrection(RenderBefore);
	return true;
}

bool FOwnerMovementPredictor::IsCommandStartStale(
	const atlas::MovementCommandFrame& Command) const
{
	if (bHasLastAckSeq && Command.server_tick < LastAckServerTick) return true;
	if (!bHasLastCommandStart) return false;
	if (Command.server_tick < LastCommandStartServerTick) return true;
	if (Command.server_tick != LastCommandStartServerTick) return false;
	if (Command.command_id == LastCommandStartId) return true;
	return bHasOpenCommandStart && Command.priority <= LastCommandStartPriority;
}

bool FOwnerMovementPredictor::IsBehindLastCommandStart(uint32 ServerTick) const
{
	return bHasLastCommandStart && ServerTick < LastCommandStartServerTick;
}

void FOwnerMovementPredictor::AdvanceActiveCommand(float DeltaTime)
{
	if (!bHasActiveCommand) return;
	ApplyCommandSample(ClampDeltaMs(DeltaTime));
}

void FOwnerMovementPredictor::ApplyCommandSample(uint32 DeltaMs)
{
	if (ActiveCommand.duration_ms == 0) return;
	const atlas::Vec3 Previous = StatePosition(PredictedState);
	if (DeltaMs > 0)
	{
		ActiveCommandElapsedMs = FMath::Min(
			ActiveCommandElapsedMs + DeltaMs, static_cast<uint32>(ActiveCommand.duration_ms));
	}
	const atlas::Vec3 Next = SampleMovementCommandPosition(ActiveCommand, ActiveCommandElapsedMs);
	PredictedState.position_x = Next.x;
	PredictedState.position_y = Next.y;
	PredictedState.position_z = Next.z;
	if (DeltaMs > 0)
	{
		const float InvDt = 1000.0f / static_cast<float>(DeltaMs);
		const atlas::Vec3 Velocity = Scale(Subtract(Next, Previous), InvDt);
		PredictedState.velocity_x = Velocity.x;
		PredictedState.velocity_y = Velocity.y;
		PredictedState.velocity_z = Velocity.z;
	}
	else
	{
		PredictedState.velocity_x = 0.0f;
		PredictedState.velocity_y = 0.0f;
		PredictedState.velocity_z = 0.0f;
	}

	atlas::Vec3 Direction;
	if (TryMovementCommandDirection(ActiveCommand, Direction))
	{
		if (!AllowsTurnInput(ActiveCommand) || !bHasCommandTurnInput)
		{
			PredictedState.direction_x = Direction.x;
			PredictedState.direction_y = Direction.y;
			PredictedState.direction_z = Direction.z;
		}
	}
	if (ActiveCommandElapsedMs >= ActiveCommand.duration_ms)
	{
		bHasActiveCommand = false;
	}
}

void FOwnerMovementPredictor::ApplyCommandTurnInput(
	const AtlasMovementInputFrame& Input)
{
	const atlas::Vec3 Direction = DirectionFromInputYaw(Input);
	PredictedState.direction_x = Direction.x;
	PredictedState.direction_y = Direction.y;
	PredictedState.direction_z = Direction.z;
	PredictedState.last_processed_input_seq = Input.seq;
	bHasCommandTurnInput = true;
}

void FOwnerMovementPredictor::AlignActiveCommandToState(const atlas::Vec3& Position)
{
	if (!bHasActiveCommand) return;
	const atlas::Vec3 Path = Subtract(ActiveCommand.target_position, ActiveCommand.start_position);
	const float PathLengthSq = LengthSq(Path);
	if (PathLengthSq <= 0.0001f) return;

	const float Progress =
		FMath::Clamp(Dot(Subtract(Position, ActiveCommand.start_position), Path) /
			PathLengthSq, 0.0f, 1.0f);
	const float NormalizedTime =
		atlas::mvp::movement_curves::TimeAtProgress(ActiveCommand.curve_id, Progress);
	const uint32 ElapsedMs = static_cast<uint32>(
		FMath::RoundToInt(NormalizedTime * static_cast<float>(ActiveCommand.duration_ms)));
	if (ElapsedMs > ActiveCommandElapsedMs)
	{
		ActiveCommandElapsedMs = ElapsedMs;
	}
	if (ActiveCommandElapsedMs >= ActiveCommand.duration_ms)
	{
		bHasActiveCommand = false;
	}
}

void FOwnerMovementPredictor::ApplyVisualCorrection(const FVector& RenderBefore)
{
	const FVector Corrected = AtlasToUE(StatePosition(PredictedState));
	const float ErrorCm = static_cast<float>(FVector::Dist(RenderBefore, Corrected));
	const float ErrorM = ErrorCm / 100.0f;
	const AtlasMovementCorrectionTier Tier =
		AtlasNetMovementClassifyCorrection(ErrorM);
	if (Tier == ATLAS_MOVEMENT_CORRECTION_SNAP)
	{
		VisualOffset = FVector::ZeroVector;
		VisualOffsetDecayCmPerSec = 0.0f;
	}
	else if (Tier != ATLAS_MOVEMENT_CORRECTION_NONE)
	{
		VisualOffset = RenderBefore - Corrected;
		VisualOffsetDecayCmPerSec =
			Tier == ATLAS_MOVEMENT_CORRECTION_TIER2 ? kFastDecayCmPerSec : kSlowDecayCmPerSec;
	}
}

void FOwnerMovementPredictor::TickVisualOffset(float DeltaTime)
{
	AdvanceActiveCommand(DeltaTime);
	if (VisualOffset.SizeSquared() <= 0.01f)
	{
		VisualOffset = FVector::ZeroVector;
		return;
	}
	const float Step =
		FMath::Max(VisualOffsetDecayCmPerSec, kSlowDecayCmPerSec) * FMath::Max(DeltaTime, 0.0f);
	const float Distance = static_cast<float>(VisualOffset.Size());
	if (Distance <= Step || Distance <= KINDA_SMALL_NUMBER)
	{
		VisualOffset = FVector::ZeroVector;
		return;
	}
	VisualOffset -= VisualOffset * (Step / Distance);
}

const AtlasMovementInputFrame& FOwnerMovementPredictor::HistoryAt(int32 Index) const
{
	return InputHistory[(HistoryStart + Index) % kHistoryCapacity];
}

FVector FOwnerMovementPredictor::RenderPositionUE() const
{
	return AtlasToUE(StatePosition(PredictedState)) + VisualOffset;
}

FVector FOwnerMovementPredictor::RenderDirectionUE() const
{
	return DirectionToUE(StateDirection(PredictedState));
}

#if WITH_DEV_AUTOMATION_TESTS
const AtlasMovementInputFrame& FOwnerMovementPredictor::HistoryAtForTest(int32 Index) const
{
	return HistoryAt(Index);
}
#endif
