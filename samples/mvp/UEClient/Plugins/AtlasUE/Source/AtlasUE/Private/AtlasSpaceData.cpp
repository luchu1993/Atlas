#include "AtlasSpaceData.h"

#include <cstring>

void UAtlasSpaceData::OnSpaceDataInit(uint32_t SpaceId, uint16_t KeyId,
                                      const uint8_t* Data, std::size_t Len)
{
	Store(SpaceId, KeyId, Data, Len);
	OnChanged.Broadcast(static_cast<int32>(SpaceId), static_cast<int32>(KeyId));
}

void UAtlasSpaceData::OnSpaceDataUpdate(uint32_t SpaceId, uint16_t KeyId,
                                        const uint8_t* Data, std::size_t Len)
{
	Store(SpaceId, KeyId, Data, Len);
	OnChanged.Broadcast(static_cast<int32>(SpaceId), static_cast<int32>(KeyId));
}

void UAtlasSpaceData::OnSpaceDataDelete(uint32_t SpaceId, uint16_t KeyId)
{
	if (Values.Remove(MakeKey(SpaceId, KeyId)) > 0)
	{
		OnRemoved.Broadcast(static_cast<int32>(SpaceId), static_cast<int32>(KeyId));
	}
}

void UAtlasSpaceData::Store(uint32_t SpaceId, uint16_t KeyId,
                            const uint8_t* Data, std::size_t Len)
{
	TArray<uint8>& Buf = Values.FindOrAdd(MakeKey(SpaceId, KeyId));
	Buf.SetNumUninitialized(static_cast<int32>(Len));
	if (Len > 0 && Data != nullptr) FMemory::Memcpy(Buf.GetData(), Data, Len);
}

const TArray<uint8>* UAtlasSpaceData::Find(int32 SpaceId, int32 KeyId) const
{
	if (SpaceId < 0 || KeyId < 0 || KeyId > 0xFFFF) return nullptr;
	return Values.Find(MakeKey(static_cast<uint32_t>(SpaceId),
		static_cast<uint16_t>(KeyId)));
}

bool UAtlasSpaceData::GetInt32(int32 SpaceId, int32 KeyId, int32& OutValue) const
{
	OutValue = 0;
	const TArray<uint8>* Buf = Find(SpaceId, KeyId);
	if (Buf == nullptr || Buf->Num() < 4) return false;
	FMemory::Memcpy(&OutValue, Buf->GetData(), 4);
	return true;
}

bool UAtlasSpaceData::GetInt64(int32 SpaceId, int32 KeyId, int64& OutValue) const
{
	OutValue = 0;
	const TArray<uint8>* Buf = Find(SpaceId, KeyId);
	if (Buf == nullptr || Buf->Num() < 8) return false;
	FMemory::Memcpy(&OutValue, Buf->GetData(), 8);
	return true;
}

bool UAtlasSpaceData::GetFloat(int32 SpaceId, int32 KeyId, float& OutValue) const
{
	OutValue = 0.f;
	const TArray<uint8>* Buf = Find(SpaceId, KeyId);
	if (Buf == nullptr || Buf->Num() < 4) return false;
	FMemory::Memcpy(&OutValue, Buf->GetData(), 4);
	return true;
}

bool UAtlasSpaceData::GetBool(int32 SpaceId, int32 KeyId, bool& OutValue) const
{
	OutValue = false;
	const TArray<uint8>* Buf = Find(SpaceId, KeyId);
	if (Buf == nullptr || Buf->Num() < 1) return false;
	OutValue = (*Buf)[0] != 0;
	return true;
}

bool UAtlasSpaceData::GetString(int32 SpaceId, int32 KeyId, FString& OutValue) const
{
	OutValue.Reset();
	const TArray<uint8>* Buf = Find(SpaceId, KeyId);
	if (Buf == nullptr) return false;
	if (Buf->Num() == 0) return true;
	OutValue = FString::ConstructFromPtrSize(
		reinterpret_cast<const ANSICHAR*>(Buf->GetData()), Buf->Num());
	return true;
}

void UAtlasSpaceData::Clear()
{
	Values.Empty();
}
