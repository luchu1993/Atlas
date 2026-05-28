#pragma once

#include <algorithm>
#include <cmath>
#include <limits>

#include "UObject/WeakObjectPtr.h"

#include "AtlasAvatarView.h"
#include "AtlasCore/avatar_filter.h"
#include "AtlasCore/entity_view.h"
#include "AtlasCoordinates.h"
#include "MovementCommandCurves.h"
#include "MovementCommandMath.h"
#include "gen/Avatar.gen.h"
#include "net_client/client_api.h"

namespace MvMath = atlas::mvp::movement_math;

class FBpAvatarEntity : public atlas::mvp::Avatar
{
public:
	FBpAvatarEntity(atlas::EntityId Id, atlas::EntityTypeId Type)
		: atlas::mvp::Avatar(Id, Type) {}

	struct FMovementAck
	{
		uint32 AckedInputSeq = 0;
		uint32 ServerTick = 0;
		AtlasMovementStateFrame State{};
		uint16 CorrectionFlags = 0;
	};

	struct FMovementCommandEnd
	{
		uint32 CommandId = 0;
		uint32 ServerTick = 0;
		atlas::MovementCommandEndReason Reason =
			atlas::MovementCommandEndReason::kCompleted;
		AtlasMovementStateFrame State{};
	};

	void BindView(UAtlasAvatarView* InView)
	{
		ViewPtr = InView;
		if (InView != nullptr) InView->Bind(this);
	}

	void SetOwnerInputActive(bool Active) { bOwnerInputActive = Active; }

	// Owner input gates on this so it never snaps the actor to spawn-default
	// before the server pos has arrived.
	bool HasInitialTransform() const { return bHasInitialTransform; }

	void OnPositionReceived(double ServerTime, const atlas::Vec3& Pos,
	                        const atlas::Vec3& Dir, bool OnGround) override
	{
		LastServerPos = Pos;
		LastServerDir = Dir;
		bHasInitialTransform = true;
		if (!bOwnerInputActive && bHasOpenRemoteCommand)
		{
			Filter.Reset();
			if (bHasActiveRemoteCommand)
			{
				AlignRemoteMovementCommandToPosition(Pos);
				ApplyRemoteMovementCommandSample(0);
				if (MvMath::AllowsTurnInput(RemoteMovementCommand))
				{
					LastServerDir = Dir;
					bRemoteCommandHasServerDirection = true;
				}
				return;
			}
			bHasRemoteCommandPosition = true;
			if (atlas::EntityView* V = View())
			{
				V->OnTransformReplicated(LastServerPos, atlas::Quat{});
			}
			return;
		}
		if (!bOwnerInputActive)
		{
			bHasRemoteCommandPosition = false;
			bRemoteCommandHasServerDirection = false;
			Filter.Input(ServerTime, Pos, Dir, OnGround);
		}
	}

	const atlas::Vec3& InitialServerPos() const { return LastServerPos; }
	const atlas::Vec3& InitialServerDir() const { return LastServerDir; }

#if WITH_DEV_AUTOMATION_TESTS
	bool HasOpenRemoteCommandForTest() const { return bHasOpenRemoteCommand; }
	bool HasActiveRemoteCommandForTest() const { return bHasActiveRemoteCommand; }
	bool HasRemoteCommandPositionForTest() const { return bHasRemoteCommandPosition; }
#endif

	void OnMovementStateAck(uint32_t AckedInputSeq, uint32_t ServerTick,
	                        const AtlasMovementStateFrame& State,
	                        uint16_t CorrectionFlags) override
	{
		if (bHasPendingMovementAck &&
		    !MvMath::IsMovementAckNewer(AckedInputSeq, ServerTick,
		                                PendingMovementAck.AckedInputSeq,
		                                PendingMovementAck.ServerTick))
		{
			return;
		}
		PendingMovementAck.AckedInputSeq = AckedInputSeq;
		PendingMovementAck.ServerTick = ServerTick;
		PendingMovementAck.State = State;
		PendingMovementAck.CorrectionFlags = CorrectionFlags;
		bHasPendingMovementAck = true;
	}

	void OnMovementCommandStart(const atlas::MovementCommandFrame& Command) override
	{
		PendingMovementCommandStart = Command;
		bHasPendingMovementCommandStart = true;
		if (!bOwnerInputActive)
		{
			StartRemoteMovementCommand(Command);
		}
	}

	void OnMovementCommandEnd(uint32_t CommandId, uint32_t ServerTick,
	                          atlas::MovementCommandEndReason Reason,
	                          const AtlasMovementStateFrame& State) override
	{
		PendingMovementCommandEnd.CommandId = CommandId;
		PendingMovementCommandEnd.ServerTick = ServerTick;
		PendingMovementCommandEnd.Reason = Reason;
		PendingMovementCommandEnd.State = State;
		bHasPendingMovementCommandEnd = true;
		if (bOwnerInputActive) return;
		if (RemoteMovementCommand.command_id != 0 &&
		    RemoteMovementCommand.command_id != CommandId)
		{
			return;
		}

		RemoteMovementCommand = atlas::MovementCommandFrame{};
		bHasOpenRemoteCommand = false;
		bHasActiveRemoteCommand = false;
		bRemoteCommandHasServerDirection = false;
		LastServerPos = MvMath::StatePosition(State);
		LastServerDir = MvMath::StateDirection(State);
		bHasRemoteCommandPosition = true;
		bHasInitialTransform = true;
		Filter.Reset();
		if (atlas::EntityView* V = View())
		{
			V->OnTransformReplicated(LastServerPos, atlas::Quat{});
		}
	}

	bool ConsumeMovementAck(FMovementAck& OutAck)
	{
		if (!bHasPendingMovementAck) return false;
		OutAck = PendingMovementAck;
		bHasPendingMovementAck = false;
		return true;
	}

	bool ConsumeMovementCommandStart(atlas::MovementCommandFrame& OutCommand)
	{
		if (!bHasPendingMovementCommandStart) return false;
		OutCommand = PendingMovementCommandStart;
		bHasPendingMovementCommandStart = false;
		return true;
	}

	bool ConsumeMovementCommandEnd(FMovementCommandEnd& OutEnd)
	{
		if (!bHasPendingMovementCommandEnd) return false;
		OutEnd = PendingMovementCommandEnd;
		bHasPendingMovementCommandEnd = false;
		return true;
	}

	void TickInterpolation(double Dt) override
	{
		if (bOwnerInputActive) return;
		atlas::EntityView* V = View();
		if (V == nullptr) return;
		if (bHasActiveRemoteCommand)
		{
			const uint32 DeltaMs = MvMath::ClampDeltaMs(Dt);
			ApplyRemoteMovementCommandSample(DeltaMs);
			return;
		}
		if (bHasRemoteCommandPosition)
		{
			V->OnTransformReplicated(LastServerPos, atlas::Quat{});
			return;
		}
		Filter.UpdateLatency(Dt);
		atlas::Vec3 Pos;
		atlas::Vec3 Dir;
		bool OnGround = false;
		if (Filter.TryEvaluate(Pos, Dir, OnGround))
		{
			V->OnTransformReplicated(Pos, atlas::Quat{});
		}
	}

	void OnHpChanged(int32_t Old, int32_t New) override
	{
		if (auto* v = ViewPtr.Get()) v->NotifyHpChanged(Old, New);
	}
	void OnLevelChanged(int32_t Old, int32_t New) override
	{
		if (auto* v = ViewPtr.Get()) v->NotifyLevelChanged(Old, New);
	}
	void OnManaChanged(int32_t Old, int32_t New) override
	{
		if (auto* v = ViewPtr.Get()) v->NotifyManaChanged(Old, New);
	}
	void OnSecretChanged(const std::string& Old, const std::string& New) override
	{
		if (auto* v = ViewPtr.Get())
			v->NotifySecretChanged(FString(Old.c_str()), FString(New.c_str()));
	}

	void ShowDamage(int32_t Amount, uint32_t AttackerId) override
	{
		if (auto* v = ViewPtr.Get())
			v->NotifyShowDamage(Amount, static_cast<int32>(AttackerId));
	}
	void OnDied(uint32_t AttackerId) override
	{
		if (auto* v = ViewPtr.Get())
			v->NotifyOnDied(static_cast<int32>(AttackerId));
	}
	void OnRespawned(const atlas::Vec3& Pos) override
	{
		if (auto* v = ViewPtr.Get()) v->NotifyOnRespawned(AtlasToUE(Pos));
	}
	void OnProjectileFired(uint32_t ShotId, const atlas::Vec3& Origin,
	                       const atlas::Vec3& Velocity) override
	{
		if (auto* v = ViewPtr.Get())
			v->NotifyOnProjectileFired(static_cast<int32>(ShotId), AtlasToUE(Origin),
			                            AtlasToUE(Velocity));
	}
	void OnProjectileEnded(uint32_t ShotId, const atlas::Vec3& EndPos,
	                       uint32_t HitTargetId) override
	{
		if (auto* v = ViewPtr.Get())
			v->NotifyOnProjectileEnded(static_cast<int32>(ShotId), AtlasToUE(EndPos),
			                            static_cast<int32>(HitTargetId));
	}

private:

	bool StartRemoteMovementCommand(const atlas::MovementCommandFrame& Command)
	{
		if (!MvMath::IsMovementCommandValid(Command)) return false;
		RemoteMovementCommand = Command;
		RemoteMovementCommandElapsedMs = Command.elapsed_ms;
		bHasOpenRemoteCommand = true;
		bHasActiveRemoteCommand = true;
		bRemoteCommandHasServerDirection = false;
		Filter.Reset();
		ApplyRemoteMovementCommandSample(0);
		return true;
	}

	void ApplyRemoteMovementCommandSample(uint32 DeltaMs)
	{
		if (DeltaMs > 0)
		{
			RemoteMovementCommandElapsedMs =
				std::min(RemoteMovementCommandElapsedMs + DeltaMs,
					static_cast<uint32>(RemoteMovementCommand.duration_ms));
		}
		LastServerPos =
			MvMath::SampleMovementCommandPosition(RemoteMovementCommand,
			                                       RemoteMovementCommandElapsedMs);
		atlas::Vec3 Direction;
		if (MvMath::TryMovementCommandDirection(RemoteMovementCommand, Direction))
		{
			if (!MvMath::AllowsTurnInput(RemoteMovementCommand) || !bRemoteCommandHasServerDirection)
			{
				LastServerDir = Direction;
			}
		}
		bHasRemoteCommandPosition = true;
		if (RemoteMovementCommandElapsedMs >= RemoteMovementCommand.duration_ms)
		{
			bHasActiveRemoteCommand = false;
		}
		if (atlas::EntityView* V = View())
		{
			V->OnTransformReplicated(LastServerPos, atlas::Quat{});
		}
	}

	void AlignRemoteMovementCommandToPosition(const atlas::Vec3& Position)
	{
		if (!bHasActiveRemoteCommand) return;
		const atlas::Vec3 Path =
			MvMath::Subtract(RemoteMovementCommand.target_position,
				RemoteMovementCommand.start_position);
		const float PathLengthSq = MvMath::LengthSq(Path);
		if (PathLengthSq <= 0.0001f) return;

		const float Progress =
			std::clamp(MvMath::Dot(MvMath::Subtract(Position,
				RemoteMovementCommand.start_position), Path) / PathLengthSq, 0.0f, 1.0f);
		const float NormalizedTime = atlas::mvp::movement_curves::TimeAtProgress(
			RemoteMovementCommand.curve_id, Progress);
		const uint32 ElapsedMs = static_cast<uint32>(
			std::round(NormalizedTime *
				static_cast<float>(RemoteMovementCommand.duration_ms)));
		if (ElapsedMs > RemoteMovementCommandElapsedMs)
		{
			RemoteMovementCommandElapsedMs = ElapsedMs;
		}
		if (RemoteMovementCommandElapsedMs >= RemoteMovementCommand.duration_ms)
		{
			bHasActiveRemoteCommand = false;
		}
	}

	atlas::AvatarFilter Filter;
	TWeakObjectPtr<UAtlasAvatarView> ViewPtr;
	atlas::Vec3 LastServerPos{};
	atlas::Vec3 LastServerDir{};
	atlas::MovementCommandFrame PendingMovementCommandStart{};
	atlas::MovementCommandFrame RemoteMovementCommand{};
	uint32 RemoteMovementCommandElapsedMs = 0;
	bool bOwnerInputActive = false;
	bool bHasInitialTransform = false;
	bool bHasPendingMovementAck = false;
	bool bHasPendingMovementCommandStart = false;
	bool bHasPendingMovementCommandEnd = false;
	bool bHasOpenRemoteCommand = false;
	bool bHasActiveRemoteCommand = false;
	bool bHasRemoteCommandPosition = false;
	bool bRemoteCommandHasServerDirection = false;
	FMovementAck PendingMovementAck;
	FMovementCommandEnd PendingMovementCommandEnd;
};
