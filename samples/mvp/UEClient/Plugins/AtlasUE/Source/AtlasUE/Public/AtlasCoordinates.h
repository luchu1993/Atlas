#pragma once

#include "CoreMinimal.h"

#include "AtlasCore/entity_view.h"

// Atlas wire follows Unity (Y-up, X=right, Z=forward); UE is Z-up, X=forward,
// Y=right. Permute axes + scale meters to centimeters.
inline FVector AtlasToUE(const atlas::Vec3& AtlasPos)
{
	return FVector(AtlasPos.z * 100.0f, AtlasPos.x * 100.0f, AtlasPos.y * 100.0f);
}

inline atlas::Vec3 UEToAtlas(const FVector& UEPos)
{
	return atlas::Vec3{
		static_cast<float>(UEPos.Y / 100.0),
		static_cast<float>(UEPos.Z / 100.0),
		static_cast<float>(UEPos.X / 100.0)};
}
