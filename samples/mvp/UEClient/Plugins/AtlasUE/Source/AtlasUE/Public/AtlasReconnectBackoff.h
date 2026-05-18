#pragma once

#include "Math/UnrealMathUtility.h"

// 1s, 2s, 4s, ..., capped at 30s. Attempt 0 is the first retry.
namespace AtlasReconnect
{
constexpr double kBaseBackoffSec = 1.0;
constexpr double kMaxBackoffSec = 30.0;

inline double ComputeBackoffSec(int32 Attempts)
{
	const double Scaled = kBaseBackoffSec * FMath::Pow(2.0, static_cast<double>(Attempts));
	return FMath::Min(Scaled, kMaxBackoffSec);
}
}  // namespace AtlasReconnect
