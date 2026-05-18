#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "bp_avatar_view_test.generated.h"

// Test-only listener: a UFUNCTION shaped like FAtlasAvatarIntChanged so the
// dynamic multicast delegate can bind it via AddDynamic in
// bp_avatar_view_test.cpp. Lives in a header so UHT generates the reflection
// glue.
UCLASS()
class UAtlasBpAvatarListener : public UObject
{
	GENERATED_BODY()
public:
	int32 HpCalls = 0;
	int32 LastOld = 0;
	int32 LastNew = 0;

	int32 LevelCalls = 0;
	int32 LastLevelOld = 0;
	int32 LastLevelNew = 0;

	UFUNCTION()
	void OnHp(int32 OldValue, int32 NewValue)
	{
		++HpCalls;
		LastOld = OldValue;
		LastNew = NewValue;
	}

	UFUNCTION()
	void OnLevel(int32 OldValue, int32 NewValue)
	{
		++LevelCalls;
		LastLevelOld = OldValue;
		LastLevelNew = NewValue;
	}

	// Counters + last-args capture for the 5 Avatar.def client_methods. BP
	// game code is expected to bind these to its own UFUNCTION; the test
	// fixture just records what came through so we can assert routing +
	// type conversion (uint32→int32, atlas::Vec3→FVector via AtlasToUE).
	int32 ShowDamageCalls = 0;
	int32 LastDamageAmount = 0;
	int32 LastDamageAttackerId = 0;
	int32 OnDiedCalls = 0;
	int32 LastDiedAttackerId = 0;
	int32 OnRespawnedCalls = 0;
	FVector LastRespawnPos = FVector::ZeroVector;
	int32 OnProjectileFiredCalls = 0;
	int32 LastFiredShotId = 0;
	FVector LastFiredOrigin = FVector::ZeroVector;
	FVector LastFiredVelocity = FVector::ZeroVector;
	int32 OnProjectileEndedCalls = 0;
	int32 LastEndedShotId = 0;
	FVector LastEndedPos = FVector::ZeroVector;
	int32 LastEndedHitTargetId = 0;

	UFUNCTION()
	void HandleShowDamage(int32 Amount, int32 AttackerId)
	{
		++ShowDamageCalls;
		LastDamageAmount = Amount;
		LastDamageAttackerId = AttackerId;
	}
	UFUNCTION()
	void HandleOnDied(int32 AttackerId)
	{
		++OnDiedCalls;
		LastDiedAttackerId = AttackerId;
	}
	UFUNCTION()
	void HandleOnRespawned(FVector Pos)
	{
		++OnRespawnedCalls;
		LastRespawnPos = Pos;
	}
	UFUNCTION()
	void HandleOnProjectileFired(int32 ShotId, FVector Origin, FVector Velocity)
	{
		++OnProjectileFiredCalls;
		LastFiredShotId = ShotId;
		LastFiredOrigin = Origin;
		LastFiredVelocity = Velocity;
	}
	UFUNCTION()
	void HandleOnProjectileEnded(int32 ShotId, FVector EndPos, int32 HitTargetId)
	{
		++OnProjectileEndedCalls;
		LastEndedShotId = ShotId;
		LastEndedPos = EndPos;
		LastEndedHitTargetId = HitTargetId;
	}
};
