#pragma once

#include "CoreMinimal.h"

#include "AtlasCore/entity_view.h"

// Atlas server uses meters; UE uses centimeters. Handedness / axis order is
// observed during M0 integration and adjusted here if mismatched — until then
// only the unit scale is converted.
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
