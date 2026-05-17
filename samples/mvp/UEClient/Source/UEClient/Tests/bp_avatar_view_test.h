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
};
