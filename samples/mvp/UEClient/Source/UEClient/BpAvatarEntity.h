#pragma once

#include "UObject/WeakObjectPtr.h"

#include "AtlasAvatarView.h"
#include "AtlasCore/avatar_filter.h"
#include "AtlasCore/entity_view.h"
#include "gen/Avatar.gen.h"

// Game-side subclass that ties three concerns into the one entity that
// represents the player / NPC avatars on the wire:
//  - codegen Avatar's typed getters + RPC stubs (from gen/Avatar.gen.h)
//  - AvatarFilter-based position interpolation (mirrors MovingClientEntity)
//  - Bridge from codegen scalar change hooks to UAtlasAvatarView delegates
class FBpAvatarEntity : public atlas::mvp::Avatar
{
public:
	FBpAvatarEntity(atlas::EntityId Id, atlas::EntityTypeId Type)
		: atlas::mvp::Avatar(Id, Type) {}

	void BindView(UAtlasAvatarView* InView)
	{
		ViewPtr = InView;
		if (InView != nullptr) InView->Bind(this);
	}

	void OnPositionReceived(double ServerTime, const atlas::Vec3& Pos,
	                        const atlas::Vec3& Dir, bool OnGround) override
	{
		Filter.Input(ServerTime, Pos, Dir, OnGround);
	}

	void TickInterpolation(double Dt) override
	{
		atlas::EntityView* V = View();
		if (V == nullptr) return;
		Filter.UpdateLatency(Dt);
		atlas::Vec3 Pos;
		atlas::Vec3 Dir;
		bool OnGround = false;
		if (Filter.TryEvaluate(Pos, Dir, OnGround))
		{
			V->OnTransformReplicated(Pos, atlas::Quat{});
		}
	}

	void OnHpChanged(int32_t Old, int32_t New) override
	{
		if (auto* v = ViewPtr.Get()) v->NotifyHpChanged(Old, New);
	}
	void OnLevelChanged(int32_t Old, int32_t New) override
	{
		if (auto* v = ViewPtr.Get()) v->NotifyLevelChanged(Old, New);
	}
	void OnManaChanged(int32_t Old, int32_t New) override
	{
		if (auto* v = ViewPtr.Get()) v->NotifyManaChanged(Old, New);
	}
	void OnSecretChanged(const std::string& Old, const std::string& New) override
	{
		if (auto* v = ViewPtr.Get())
			v->NotifySecretChanged(FString(Old.c_str()), FString(New.c_str()));
	}

private:
	atlas::AvatarFilter Filter;
	TWeakObjectPtr<UAtlasAvatarView> ViewPtr;
};
