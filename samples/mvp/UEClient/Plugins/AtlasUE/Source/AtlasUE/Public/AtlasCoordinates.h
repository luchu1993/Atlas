#pragma once

#include "CoreMinimal.h"

#include "AtlasCore/entity_view.h"

// Atlas wire is meters; UE is centimeters. Handedness / axis-order conversion
// hasn't been needed yet — add it here when first observed mismatched.
inline FVector AtlasToUE(const atlas::Vec3& AtlasPos)
{
	return FVector(AtlasPos.x * 100.0f, AtlasPos.y * 100.0f, AtlasPos.z * 100.0f);
}

inline atlas::Vec3 UEToAtlas(const FVector& UEPos)
{
	return atlas::Vec3{
		static_cast<float>(UEPos.X / 100.0),
		static_cast<float>(UEPos.Y / 100.0),
		static_cast<float>(UEPos.Z / 100.0)};
}
