#pragma once

#include "CoreMinimal.h"

#include <array>

#include "AtlasCore/client_entity.h"
#include "net_client/client_api.h"

class FOwnerMovementPredictor
{
public:
	static constexpr int32 kHistoryCapacity = 96;

	void Reset(const atlas::Vec3& Position, const atlas::Vec3& Direction);

	bool PushInput(const AtlasMovementInputFrame& Input);
	int32 CopyRecentFrames(AtlasMovementInputFrame* OutFrames, int32 Capacity) const;

	bool ApplyMovementAck(uint32 AckedInputSeq, uint32 ServerTick,
	                      const AtlasMovementStateFrame& State,
	                      float& OutErrorM, uint16& OutCorrectionFlags);
	bool ApplyMovementCommandStart(const atlas::MovementCommandFrame& Command);
	bool ApplyMovementCommandEnd(uint32 CommandId, uint32 ServerTick,
	                             atlas::MovementCommandEndReason Reason,
	                             const AtlasMovementStateFrame& State);

	void TickVisualOffset(float DeltaTime);

	AtlasMovementInputFrame BuildInputFrame(
		float Forward, float Right, float CamYaw, uint16 InputDtMs);
	[[nodiscard]] bool HasActiveCommand() const { return bHasActiveCommand; }
	[[nodiscard]] bool AcceptsInput() const;
	[[nodiscard]] FVector RenderPositionUE() const;
	[[nodiscard]] FVector RenderDirectionUE() const;

	static bool IsInputSeqNewer(uint32 Seq, uint32 Previous);
	static bool IsMovementAckNewer(uint32 AckedInputSeq, uint32 ServerTick,
	                               uint32 LastAckSeq, uint32 LastServerTick);
	static uint32 SeedNextInputSeqFromAck(uint32 NextInputSeq, uint32 AckedInputSeq);

#if WITH_DEV_AUTOMATION_TESTS
	[[nodiscard]] const AtlasMovementStateFrame& StateForTest() const { return PredictedState; }
	[[nodiscard]] uint32 LastAckSeqForTest() const { return LastAckSeq; }
	[[nodiscard]] atlas::MovementCommandEndReason LastCommandEndReasonForTest() const
	{
		return LastCommandEndReason;
	}
	[[nodiscard]] int32 HistoryCountForTest() const { return HistoryCount; }
	[[nodiscard]] const AtlasMovementInputFrame& HistoryAtForTest(int32 Index) const;
	void StoreInputForTest(const AtlasMovementInputFrame& Input) { StoreInput(Input); }
	void DropAckedForTest(uint32 AckedInputSeq) { DropAcked(AckedInputSeq); }
	void AdvanceActiveCommandForTest(float DeltaTime) { AdvanceActiveCommand(DeltaTime); }
#endif

private:
	bool ApplyInputForPrediction(const AtlasMovementInputFrame& Input, uint32 ServerTick);
	bool PredictStep(const AtlasMovementInputFrame& Input, uint32 ServerTick);
	void ApplyCommandTurnInput(const AtlasMovementInputFrame& Input);
	void StoreInput(const AtlasMovementInputFrame& Input);
	void DropAcked(uint32 AckedInputSeq);
	void ReplayPending();
	void AdvanceActiveCommand(float DeltaTime);
	void ApplyCommandSample(uint32 DeltaMs);
	void AlignActiveCommandToState(const atlas::Vec3& Position);
	void ApplyVisualCorrection(const FVector& RenderBefore);
	[[nodiscard]] bool IsCommandStartStale(const atlas::MovementCommandFrame& Command) const;
	[[nodiscard]] bool IsBehindLastCommandStart(uint32 ServerTick) const;
	[[nodiscard]] const AtlasMovementInputFrame& HistoryAt(int32 Index) const;

	AtlasMovementStateFrame PredictedState{};
	std::array<AtlasMovementInputFrame, kHistoryCapacity> InputHistory{};
	atlas::MovementCommandFrame ActiveCommand{};
	atlas::MovementCommandEndReason LastCommandEndReason =
		atlas::MovementCommandEndReason::kCompleted;
	FVector VisualOffset = FVector::ZeroVector;
	float VisualOffsetDecayCmPerSec = 0.0f;
	int32 HistoryStart = 0;
	int32 HistoryCount = 0;
	uint32 LastAckSeq = 0;
	uint32 LastAckServerTick = 0;
	uint32 LastCommandStartServerTick = 0;
	uint32 LastCommandStartId = 0;
	uint32 NextInputSeq = 1;
	uint32 InputTick = 1;
	uint32 ActiveCommandElapsedMs = 0;
	uint8 LastCommandStartPriority = 0;
	bool bHasLastAckSeq = false;
	bool bHasLastCommandStart = false;
	bool bHasOpenCommandStart = false;
	bool bHasActiveCommand = false;
	bool bHasCommandTurnInput = false;
};
