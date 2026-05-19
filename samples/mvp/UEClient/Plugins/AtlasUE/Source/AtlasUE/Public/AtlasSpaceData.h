#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "AtlasCore/space_data_sink.h"

#include "AtlasSpaceData.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FAtlasOnSpaceDataChanged,
	int32, SpaceId, int32, KeyId);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FAtlasOnSpaceDataRemoved,
	int32, SpaceId, int32, KeyId);

// Per-Subsystem store for server-broadcast SpaceData (envelope kinds 5/6/7).
// Implements atlas::SpaceDataSink so the AoI decoder can hand updates in.
UCLASS()
class ATLASUE_API UAtlasSpaceData : public UObject, public atlas::SpaceDataSink
{
	GENERATED_BODY()

public:
	// atlas::SpaceDataSink — called from the game thread inside TickGameThread.
	virtual void OnSpaceDataInit(uint32_t SpaceId, uint16_t KeyId,
	                             const uint8_t* Data, std::size_t Len) override;
	virtual void OnSpaceDataUpdate(uint32_t SpaceId, uint16_t KeyId,
	                               const uint8_t* Data, std::size_t Len) override;
	virtual void OnSpaceDataDelete(uint32_t SpaceId, uint16_t KeyId) override;

	// Returns false (and 0/empty) when the key is absent or the payload is
	// shorter than the requested type. Matches the Unity SpaceDataManager
	// fallback semantics tests pin on.
	UFUNCTION(BlueprintPure, Category="Atlas|SpaceData")
	bool GetInt32(int32 SpaceId, int32 KeyId, int32& OutValue) const;

	UFUNCTION(BlueprintPure, Category="Atlas|SpaceData")
	bool GetInt64(int32 SpaceId, int32 KeyId, int64& OutValue) const;

	UFUNCTION(BlueprintPure, Category="Atlas|SpaceData")
	bool GetFloat(int32 SpaceId, int32 KeyId, float& OutValue) const;

	UFUNCTION(BlueprintPure, Category="Atlas|SpaceData")
	bool GetBool(int32 SpaceId, int32 KeyId, bool& OutValue) const;

	UFUNCTION(BlueprintPure, Category="Atlas|SpaceData")
	bool GetString(int32 SpaceId, int32 KeyId, FString& OutValue) const;

	// Drops every stored key — call on reconnect / state reset so a stale
	// NpcCount from the previous session can't bleed into the next.
	UFUNCTION(BlueprintCallable, Category="Atlas|SpaceData")
	void Clear();

	UPROPERTY(BlueprintAssignable, Category="Atlas|SpaceData")
	FAtlasOnSpaceDataChanged OnChanged;

	UPROPERTY(BlueprintAssignable, Category="Atlas|SpaceData")
	FAtlasOnSpaceDataRemoved OnRemoved;

private:
	// 64-bit composite key = (SpaceId << 16) | KeyId. SpaceId fits in 32 bits
	// in practice (SpaceID is uint32_t) but a few high bits get truncated;
	// MVP uses a single space so collisions are non-issues.
	static uint64 MakeKey(uint32_t SpaceId, uint16_t KeyId)
	{
		return (static_cast<uint64>(SpaceId) << 16) | static_cast<uint64>(KeyId);
	}

	const TArray<uint8>* Find(int32 SpaceId, int32 KeyId) const;

	void Store(uint32_t SpaceId, uint16_t KeyId, const uint8_t* Data, std::size_t Len);

	TMap<uint64, TArray<uint8>> Values;
};
