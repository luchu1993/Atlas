#pragma once

#include "AtlasCore/avatar_filter.h"
#include "AtlasCore/client_entity.h"
#include "AtlasCore/entity_view.h"

// Hand-written placeholder; replaced by codegen output. Feeds AvatarFilter
// from position updates and emits interpolated transforms via the attached view.
class FAvatarEntityStub : public atlas::ClientEntity
{
public:
	FAvatarEntityStub(atlas::EntityId Id, atlas::EntityTypeId Type)
		: atlas::ClientEntity(Id, Type) {}

	void OnPositionReceived(double ServerTime, const atlas::Vec3& Pos,
	                        const atlas::Vec3& Dir, bool OnGround) override
	{
		Filter.Input(ServerTime, Pos, Dir, OnGround);
	}

	void TickInterpolation(double Dt) override
	{
		auto* V = View();
		if (!V) return;
		Filter.UpdateLatency(Dt);
		atlas::Vec3 Pos;
		atlas::Vec3 Dir;
		bool OnGround = false;
		if (Filter.TryEvaluate(Pos, Dir, OnGround))
		{
			V->OnTransformReplicated(Pos, atlas::Quat{});
		}
	}

	[[nodiscard]] const atlas::AvatarFilter& GetFilter() const { return Filter; }

private:
	atlas::AvatarFilter Filter;
};
