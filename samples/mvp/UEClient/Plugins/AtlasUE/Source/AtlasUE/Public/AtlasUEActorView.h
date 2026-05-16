#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "UObject/WeakObjectPtr.h"

#include "AtlasCore/entity_view.h"

// Bridges Atlas transform updates to a UE Actor via a weak ref so the Actor
// can vanish mid-frame without dangling the underlying ClientEntity.
class FAtlasUEActorView : public atlas::EntityView
{
public:
	explicit FAtlasUEActorView(TWeakObjectPtr<AActor> InActor) : Actor(InActor) {}

	void OnTransformReplicated(const atlas::Vec3& Position,
	                           const atlas::Quat& Rotation) override;
	void OnPropertyChanged(uint16_t /*FieldId*/) override {}

	[[nodiscard]] AActor* GetActor() const { return Actor.Get(); }

private:
	TWeakObjectPtr<AActor> Actor;
};
