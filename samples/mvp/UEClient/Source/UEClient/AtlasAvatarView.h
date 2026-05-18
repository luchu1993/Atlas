#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"

#include "AtlasAvatarView.generated.h"

namespace atlas::mvp { class Avatar; }

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(
	FAtlasAvatarIntChanged, int32, OldValue, int32, NewValue);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(
	FAtlasAvatarStringChanged, const FString&, OldValue, const FString&, NewValue);

// Avatar.def client_methods → BP. uint32 args go through int32 because
// dynamic multicast delegate reflection rejects unsigned types; the
// engine entity_id space is < 2^31 so the cast is lossless. atlas::Vec3
// converts to FVector via AtlasToUE so BP sees UE-native coords.
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(
	FAtlasAvatarShowDamage, int32, Amount, int32, AttackerId);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
	FAtlasAvatarOnDied, int32, AttackerId);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
	FAtlasAvatarOnRespawned, FVector, Pos);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(
	FAtlasAvatarOnProjectileFired, int32, ShotId, FVector, Origin, FVector, Velocity);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(
	FAtlasAvatarOnProjectileEnded, int32, ShotId, FVector, EndPos, int32, HitTargetId);

// Blueprint-facing facade around the pure-C++ codegen-emitted Avatar entity.
// Lives as a UActorComponent on AAvatarCapsule (or any actor that wants the
// BP API); the game-mode's factory links this view to the matching entity at
// spawn time. Getters read directly off the bound entity so BP always sees
// the same state as C++ — no shadow caching to keep coherent.
UCLASS(ClassGroup=(Atlas), meta=(BlueprintSpawnableComponent))
class UAtlasAvatarView : public UActorComponent
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable, Category="Atlas|Avatar")
	FAtlasAvatarIntChanged OnHpChanged;

	UPROPERTY(BlueprintAssignable, Category="Atlas|Avatar")
	FAtlasAvatarIntChanged OnLevelChanged;

	UPROPERTY(BlueprintAssignable, Category="Atlas|Avatar")
	FAtlasAvatarIntChanged OnManaChanged;

	UPROPERTY(BlueprintAssignable, Category="Atlas|Avatar")
	FAtlasAvatarStringChanged OnSecretChanged;

	UPROPERTY(BlueprintAssignable, Category="Atlas|Avatar|Rpc")
	FAtlasAvatarShowDamage OnShowDamage;

	UPROPERTY(BlueprintAssignable, Category="Atlas|Avatar|Rpc")
	FAtlasAvatarOnDied OnDied;

	UPROPERTY(BlueprintAssignable, Category="Atlas|Avatar|Rpc")
	FAtlasAvatarOnRespawned OnRespawned;

	UPROPERTY(BlueprintAssignable, Category="Atlas|Avatar|Rpc")
	FAtlasAvatarOnProjectileFired OnProjectileFired;

	UPROPERTY(BlueprintAssignable, Category="Atlas|Avatar|Rpc")
	FAtlasAvatarOnProjectileEnded OnProjectileEnded;

	UFUNCTION(BlueprintCallable, Category="Atlas|Avatar")
	int32 GetHp() const;
	UFUNCTION(BlueprintCallable, Category="Atlas|Avatar")
	int32 GetLevel() const;
	UFUNCTION(BlueprintCallable, Category="Atlas|Avatar")
	int32 GetMana() const;
	UFUNCTION(BlueprintCallable, Category="Atlas|Avatar")
	int32 GetModelId() const;
	UFUNCTION(BlueprintCallable, Category="Atlas|Avatar")
	FString GetSecret() const;

	// Linkage helpers — called from the game-mode factory once both
	// sides exist. Bind(null) safely detaches.
	void Bind(atlas::mvp::Avatar* InEntity) { Entity = InEntity; }
	[[nodiscard]] atlas::mvp::Avatar* GetEntity() const { return Entity; }

	// Bridge callbacks from FBpAvatarEntity's virtual overrides.
	void NotifyHpChanged(int32 Old, int32 New) { OnHpChanged.Broadcast(Old, New); }
	void NotifyLevelChanged(int32 Old, int32 New) { OnLevelChanged.Broadcast(Old, New); }
	void NotifyManaChanged(int32 Old, int32 New) { OnManaChanged.Broadcast(Old, New); }
	void NotifySecretChanged(const FString& Old, const FString& New) {
		OnSecretChanged.Broadcast(Old, New);
	}
	void NotifyShowDamage(int32 Amount, int32 AttackerId) {
		OnShowDamage.Broadcast(Amount, AttackerId);
	}
	void NotifyOnDied(int32 AttackerId) { OnDied.Broadcast(AttackerId); }
	void NotifyOnRespawned(const FVector& Pos) { OnRespawned.Broadcast(Pos); }
	void NotifyOnProjectileFired(int32 ShotId, const FVector& Origin, const FVector& Velocity) {
		OnProjectileFired.Broadcast(ShotId, Origin, Velocity);
	}
	void NotifyOnProjectileEnded(int32 ShotId, const FVector& EndPos, int32 HitTargetId) {
		OnProjectileEnded.Broadcast(ShotId, EndPos, HitTargetId);
	}

private:
	atlas::mvp::Avatar* Entity = nullptr;
};
