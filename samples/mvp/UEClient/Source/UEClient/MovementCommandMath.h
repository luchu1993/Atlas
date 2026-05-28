#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

#include "AtlasCore/client_entity.h"
#include "MovementCommandCurves.h"
#include "net_client/client_api.h"

namespace atlas::mvp::movement_math
{
inline constexpr uint8_t kMovementCommandInputAllowTurn = 1;

inline atlas::Vec3 StatePosition(const AtlasMovementStateFrame& State)
{
	return atlas::Vec3{State.position_x, State.position_y, State.position_z};
}

inline atlas::Vec3 StateDirection(const AtlasMovementStateFrame& State)
{
	return atlas::Vec3{State.direction_x, State.direction_y, State.direction_z};
}

inline bool IsFiniteVec3(const atlas::Vec3& Value)
{
	return std::isfinite(Value.x) && std::isfinite(Value.y) && std::isfinite(Value.z);
}

inline float LengthSq(const atlas::Vec3& Value)
{
	return Value.x * Value.x + Value.y * Value.y + Value.z * Value.z;
}

inline float Dot(const atlas::Vec3& A, const atlas::Vec3& B)
{
	return A.x * B.x + A.y * B.y + A.z * B.z;
}

inline atlas::Vec3 Subtract(const atlas::Vec3& A, const atlas::Vec3& B)
{
	return atlas::Vec3{A.x - B.x, A.y - B.y, A.z - B.z};
}

inline atlas::Vec3 Scale(const atlas::Vec3& Value, float Scalar)
{
	return atlas::Vec3{Value.x * Scalar, Value.y * Scalar, Value.z * Scalar};
}

inline atlas::Vec3 Lerp(const atlas::Vec3& A, const atlas::Vec3& B, float Alpha)
{
	return atlas::Vec3{
		A.x + (B.x - A.x) * Alpha,
		A.y + (B.y - A.y) * Alpha,
		A.z + (B.z - A.z) * Alpha};
}

inline bool IsMovementCommandValid(const atlas::MovementCommandFrame& Command)
{
	return Command.duration_ms > 0 &&
		Command.elapsed_ms <= Command.duration_ms &&
		Command.input_policy <= kMovementCommandInputAllowTurn &&
		IsFiniteVec3(Command.start_position) &&
		IsFiniteVec3(Command.target_position);
}

inline bool AllowsTurnInput(const atlas::MovementCommandFrame& Command)
{
	return Command.input_policy == kMovementCommandInputAllowTurn;
}

inline atlas::Vec3 SampleMovementCommandPosition(
	const atlas::MovementCommandFrame& Command, uint32_t ElapsedMs)
{
	const float NormalizedTime = std::clamp(
		static_cast<float>(ElapsedMs) / static_cast<float>(Command.duration_ms), 0.0f, 1.0f);
	const float Progress =
		atlas::mvp::movement_curves::Sample(Command.curve_id, NormalizedTime);
	return Lerp(Command.start_position, Command.target_position, Progress);
}

inline bool TryMovementCommandDirection(const atlas::MovementCommandFrame& Command,
                                        atlas::Vec3& Direction)
{
	const atlas::Vec3 Path = Subtract(Command.target_position, Command.start_position);
	const float PathLengthSq = LengthSq(Path);
	if (PathLengthSq <= 0.0001f) return false;
	Direction = Scale(Path, 1.0f / std::sqrt(PathLengthSq));
	return true;
}

inline uint32_t ClampDeltaMs(double DeltaSeconds)
{
	if (!std::isfinite(DeltaSeconds) || DeltaSeconds <= 0.0) return 0;
	const double Rounded = std::round(DeltaSeconds * 1000.0);
	const double MaxMs = static_cast<double>(std::numeric_limits<uint16_t>::max());
	return static_cast<uint32_t>(std::min(Rounded, MaxMs));
}

inline bool IsInputSeqNewer(uint32_t Seq, uint32_t Previous)
{
	const uint32_t Delta = Seq - Previous;
	return Delta != 0 && Delta < 0x80000000u;
}

inline bool IsMovementAckNewer(uint32_t AckedInputSeq, uint32_t ServerTick,
                                uint32_t LastAckSeq, uint32_t LastServerTick)
{
	if (IsInputSeqNewer(AckedInputSeq, LastAckSeq)) return true;
	return AckedInputSeq == LastAckSeq && ServerTick > LastServerTick;
}

inline bool IsAckStale(uint32_t AckedSeq, uint32_t ServerTick, uint32_t LastAckSeq,
                        uint32_t LastServerTick, bool bHasLastAck)
{
	if (!bHasLastAck) return false;
	if (IsInputSeqNewer(AckedSeq, LastAckSeq)) return false;
	return AckedSeq != LastAckSeq || ServerTick <= LastServerTick;
}
}
