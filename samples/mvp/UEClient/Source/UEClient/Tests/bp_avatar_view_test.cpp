#include "Tests/bp_avatar_view_test.h"

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

#include <cstdint>
#include <cstring>
#include <vector>

#include "AtlasAvatarView.h"
#include "AtlasCore/span_reader.h"
#include "AtlasUE.h"
#include "BpAvatarEntity.h"
#include "entitydef/entitydef_api.h"

namespace
{
template <typename T>
void AppendBp(std::vector<uint8_t>& buf, T v)
{
	const auto offset = buf.size();
	buf.resize(offset + sizeof(T));
	std::memcpy(buf.data() + offset, &v, sizeof(T));
}
}  // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAtlasBpAvatarViewTest,
	"Atlas.BpView.AvatarDelegatesFireOnDelta",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAtlasBpAvatarViewTest::RunTest(const FString&)
{
	AtlasEdrContext* Ctx = FAtlasUEModule::GetEdrContext();
	if (!TestNotNull(TEXT("edr context"), Ctx)) return false;
	const AtlasEdrEntity* Desc = AtlasEdrFindEntityByName(Ctx, "Avatar");
	if (!TestNotNull(TEXT("avatar descriptor"), Desc)) return false;

	UAtlasAvatarView* View = NewObject<UAtlasAvatarView>();
	UAtlasBpAvatarListener* Listener = NewObject<UAtlasBpAvatarListener>();
	View->OnHpChanged.AddDynamic(Listener, &UAtlasBpAvatarListener::OnHp);
	View->OnLevelChanged.AddDynamic(Listener, &UAtlasBpAvatarListener::OnLevel);

	FBpAvatarEntity Avatar(7, atlas::mvp::Avatar::kTypeId);
	Avatar.BindDescriptor(Desc, Ctx);
	Avatar.BindView(View);
	TestEqual(TEXT("view bound to entity"), View->GetEntity(),
		static_cast<atlas::mvp::Avatar*>(&Avatar));

	// First delta: Hp=120, Level=3 → both delegates fire once with 0→new.
	{
		std::vector<uint8_t> Buf;
		AppendBp<uint8_t>(Buf, 0x01);
		AppendBp<uint8_t>(Buf, (1 << 0) | (1 << 4));  // Hp + Level bits
		AppendBp<int32_t>(Buf, 120);
		AppendBp<int32_t>(Buf, 3);
		atlas::SpanReader R(Buf.data(), Buf.size());
		TestTrue(TEXT("apply delta 1"), Avatar.ApplyDelta(R));
		TestEqual(TEXT("hp delegate fired"), Listener->HpCalls, 1);
		TestEqual(TEXT("hp old"), Listener->LastOld, 0);
		TestEqual(TEXT("hp new"), Listener->LastNew, 120);
		TestEqual(TEXT("level delegate fired"), Listener->LevelCalls, 1);
		TestEqual(TEXT("level new"), Listener->LastLevelNew, 3);
		// BP getter mirrors current entity state.
		TestEqual(TEXT("view GetHp matches"), View->GetHp(), 120);
		TestEqual(TEXT("view GetLevel matches"), View->GetLevel(), 3);
	}

	// Second delta: Hp dirty bit set but value unchanged (120) → no delegate.
	{
		std::vector<uint8_t> Buf;
		AppendBp<uint8_t>(Buf, 0x01);
		AppendBp<uint8_t>(Buf, 1 << 0);
		AppendBp<int32_t>(Buf, 120);
		atlas::SpanReader R(Buf.data(), Buf.size());
		TestTrue(TEXT("apply delta 2"), Avatar.ApplyDelta(R));
		TestEqual(TEXT("hp delegate not refired"), Listener->HpCalls, 1);
	}

	// Detach: BindView(null) drops the link; subsequent deltas don't fire.
	{
		Avatar.BindView(nullptr);
		std::vector<uint8_t> Buf;
		AppendBp<uint8_t>(Buf, 0x01);
		AppendBp<uint8_t>(Buf, 1 << 0);
		AppendBp<int32_t>(Buf, 99);
		atlas::SpanReader R(Buf.data(), Buf.size());
		TestTrue(TEXT("apply delta 3"), Avatar.ApplyDelta(R));
		TestEqual(TEXT("hp delegate silent after detach"), Listener->HpCalls, 1);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAtlasBpAvatarRpcViewTest,
	"Atlas.BpView.AvatarClientRpcDelegatesFire",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAtlasBpAvatarRpcViewTest::RunTest(const FString&)
{
	AtlasEdrContext* Ctx = FAtlasUEModule::GetEdrContext();
	if (!TestNotNull(TEXT("edr context"), Ctx)) return false;
	const AtlasEdrEntity* Desc = AtlasEdrFindEntityByName(Ctx, "Avatar");
	if (!TestNotNull(TEXT("avatar descriptor"), Desc)) return false;

	UAtlasAvatarView* View = NewObject<UAtlasAvatarView>();
	UAtlasBpAvatarListener* Listener = NewObject<UAtlasBpAvatarListener>();
	View->OnShowDamage.AddDynamic(Listener, &UAtlasBpAvatarListener::HandleShowDamage);
	View->OnDied.AddDynamic(Listener, &UAtlasBpAvatarListener::HandleOnDied);
	View->OnRespawned.AddDynamic(Listener, &UAtlasBpAvatarListener::HandleOnRespawned);
	View->OnProjectileFired.AddDynamic(Listener,
		&UAtlasBpAvatarListener::HandleOnProjectileFired);
	View->OnProjectileEnded.AddDynamic(Listener,
		&UAtlasBpAvatarListener::HandleOnProjectileEnded);

	FBpAvatarEntity Avatar(13, atlas::mvp::Avatar::kTypeId);
	Avatar.BindDescriptor(Desc, Ctx);
	Avatar.BindView(View);

	// ShowDamage(amount=-25, attackerId=0x10000007) → both int32 args wire-
	// encoded, fixed-width. Negative amount checks signed int32 round-trip.
	{
		std::vector<uint8_t> Buf;
		AppendBp<int32_t>(Buf, -25);
		AppendBp<uint32_t>(Buf, 0x10000007u);
		atlas::SpanReader R(Buf.data(), Buf.size());
		TestTrue(TEXT("dispatch ShowDamage"),
			Avatar.DispatchRpc(atlas::mvp::Avatar::kRpcId_ShowDamage, 0, R));
		TestEqual(TEXT("ShowDamage fired once"), Listener->ShowDamageCalls, 1);
		TestEqual(TEXT("ShowDamage amount"), Listener->LastDamageAmount, -25);
		TestEqual(TEXT("ShowDamage attacker"), Listener->LastDamageAttackerId,
			static_cast<int32>(0x10000007u));
	}

	// OnDied(attackerId=42).
	{
		std::vector<uint8_t> Buf;
		AppendBp<uint32_t>(Buf, 42u);
		atlas::SpanReader R(Buf.data(), Buf.size());
		TestTrue(TEXT("dispatch OnDied"),
			Avatar.DispatchRpc(atlas::mvp::Avatar::kRpcId_OnDied, 0, R));
		TestEqual(TEXT("OnDied fired"), Listener->OnDiedCalls, 1);
		TestEqual(TEXT("OnDied attacker"), Listener->LastDiedAttackerId, 42);
	}

	// OnRespawned(pos = atlas Vec3{1, 2, 3}) → UE FVector{3*100, 1*100, 2*100}
	// after axis permute + meters→cm via AtlasToUE.
	{
		std::vector<uint8_t> Buf;
		AppendBp<float>(Buf, 1.0f);
		AppendBp<float>(Buf, 2.0f);
		AppendBp<float>(Buf, 3.0f);
		atlas::SpanReader R(Buf.data(), Buf.size());
		TestTrue(TEXT("dispatch OnRespawned"),
			Avatar.DispatchRpc(atlas::mvp::Avatar::kRpcId_OnRespawned, 0, R));
		TestEqual(TEXT("OnRespawned fired"), Listener->OnRespawnedCalls, 1);
		TestTrue(TEXT("OnRespawned pos converted"),
			Listener->LastRespawnPos.Equals(FVector(300.0, 100.0, 200.0), 0.001));
	}

	// OnProjectileFired(shot=7, origin=Vec3{0,0,0}, velocity=Vec3{0.5,0,0}).
	{
		std::vector<uint8_t> Buf;
		AppendBp<uint32_t>(Buf, 7u);
		AppendBp<float>(Buf, 0.0f); AppendBp<float>(Buf, 0.0f); AppendBp<float>(Buf, 0.0f);
		AppendBp<float>(Buf, 0.5f); AppendBp<float>(Buf, 0.0f); AppendBp<float>(Buf, 0.0f);
		atlas::SpanReader R(Buf.data(), Buf.size());
		TestTrue(TEXT("dispatch OnProjectileFired"),
			Avatar.DispatchRpc(atlas::mvp::Avatar::kRpcId_OnProjectileFired, 0, R));
		TestEqual(TEXT("Fired delegate fired"), Listener->OnProjectileFiredCalls, 1);
		TestEqual(TEXT("Fired shot id"), Listener->LastFiredShotId, 7);
		TestTrue(TEXT("Fired velocity converted"),
			Listener->LastFiredVelocity.Equals(FVector(0.0, 50.0, 0.0), 0.001));
	}

	// OnProjectileEnded(shot=7, endPos=Vec3{4,5,6}, hit=99).
	{
		std::vector<uint8_t> Buf;
		AppendBp<uint32_t>(Buf, 7u);
		AppendBp<float>(Buf, 4.0f); AppendBp<float>(Buf, 5.0f); AppendBp<float>(Buf, 6.0f);
		AppendBp<uint32_t>(Buf, 99u);
		atlas::SpanReader R(Buf.data(), Buf.size());
		TestTrue(TEXT("dispatch OnProjectileEnded"),
			Avatar.DispatchRpc(atlas::mvp::Avatar::kRpcId_OnProjectileEnded, 0, R));
		TestEqual(TEXT("Ended fired"), Listener->OnProjectileEndedCalls, 1);
		TestEqual(TEXT("Ended shot id"), Listener->LastEndedShotId, 7);
		TestEqual(TEXT("Ended hit id"), Listener->LastEndedHitTargetId, 99);
		TestTrue(TEXT("Ended pos converted"),
			Listener->LastEndedPos.Equals(FVector(600.0, 400.0, 500.0), 0.001));
	}

	// Detach safety: BindView(nullptr) means subsequent dispatch is a no-op
	// for the BP layer (entity virtual default is empty body, so DispatchRpc
	// still returns true — we just check no delegate fires).
	Avatar.BindView(nullptr);
	{
		std::vector<uint8_t> Buf;
		AppendBp<uint32_t>(Buf, 100u);
		atlas::SpanReader R(Buf.data(), Buf.size());
		TestTrue(TEXT("dispatch OnDied after detach"),
			Avatar.DispatchRpc(atlas::mvp::Avatar::kRpcId_OnDied, 0, R));
		TestEqual(TEXT("OnDied silent after detach"), Listener->OnDiedCalls, 1);
	}

	return true;
}

#endif  // WITH_DEV_AUTOMATION_TESTS
