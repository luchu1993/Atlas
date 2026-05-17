#include "AtlasAvatarView.h"

#include "gen/Avatar.gen.h"

int32 UAtlasAvatarView::GetHp() const { return Entity ? Entity->Hp() : 0; }
int32 UAtlasAvatarView::GetLevel() const { return Entity ? Entity->Level() : 0; }
int32 UAtlasAvatarView::GetMana() const { return Entity ? Entity->Mana() : 0; }
int32 UAtlasAvatarView::GetModelId() const { return Entity ? Entity->ModelId() : 0; }

FString UAtlasAvatarView::GetSecret() const
{
	if (!Entity) return FString();
	return FString(Entity->Secret().c_str());
}
