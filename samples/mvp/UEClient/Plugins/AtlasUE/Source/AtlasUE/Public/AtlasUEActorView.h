#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "UObject/WeakObjectPtr.h"

#include "AtlasCore/entity_view.h"

// Bridges Atlas entity transform updates to a UE Actor. Holds a weak reference
// so an Actor destroyed mid-frame (PIE stop, streaming unload) is silently
// dropped — the underlying atlas::ClientEntity outlives every view binding.
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
