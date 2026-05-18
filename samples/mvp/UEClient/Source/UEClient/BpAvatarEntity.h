#pragma once

#include "UObject/WeakObjectPtr.h"

#include "AtlasAvatarView.h"
#include "AtlasCore/avatar_filter.h"
#include "AtlasCore/entity_view.h"
#include "AtlasCoordinates.h"
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

	// Owner-input mode: while set, TickInterpolation skips the AvatarFilter
	// step so the input controller's client-authoritative SetActorLocation
	// isn't overwritten each frame by server-replicated transforms.
	void SetOwnerInputActive(bool Active) { bOwnerInputActive = Active; }

	void OnPositionReceived(double ServerTime, const atlas::Vec3& Pos,
	                        const atlas::Vec3& Dir, bool OnGround) override
	{
		// Owner ignores server-sent transforms; its position is the local
		// simulation. Keep feeding the filter anyway so a future switch to
		// non-owner has a warm baseline.
		Filter.Input(ServerTime, Pos, Dir, OnGround);
	}

	void TickInterpolation(double Dt) override
	{
		if (bOwnerInputActive) return;
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

	void ShowDamage(int32_t Amount, uint32_t AttackerId) override
	{
		if (auto* v = ViewPtr.Get())
			v->NotifyShowDamage(Amount, static_cast<int32>(AttackerId));
	}
	void OnDied(uint32_t AttackerId) override
	{
		if (auto* v = ViewPtr.Get())
			v->NotifyOnDied(static_cast<int32>(AttackerId));
	}
	void OnRespawned(const atlas::Vec3& Pos) override
	{
		if (auto* v = ViewPtr.Get()) v->NotifyOnRespawned(AtlasToUE(Pos));
	}
	void OnProjectileFired(uint32_t ShotId, const atlas::Vec3& Origin,
	                       const atlas::Vec3& Velocity) override
	{
		if (auto* v = ViewPtr.Get())
			v->NotifyOnProjectileFired(static_cast<int32>(ShotId), AtlasToUE(Origin),
			                            AtlasToUE(Velocity));
	}
	void OnProjectileEnded(uint32_t ShotId, const atlas::Vec3& EndPos,
	                       uint32_t HitTargetId) override
	{
		if (auto* v = ViewPtr.Get())
			v->NotifyOnProjectileEnded(static_cast<int32>(ShotId), AtlasToUE(EndPos),
			                            static_cast<int32>(HitTargetId));
	}

private:
	atlas::AvatarFilter Filter;
	TWeakObjectPtr<UAtlasAvatarView> ViewPtr;
	bool bOwnerInputActive = false;
};
