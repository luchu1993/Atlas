#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include <cstdint>
#include <cstring>
#include <vector>

#include "AtlasCore/entity_id.h"
#include "AtlasCore/rpc_sender.h"
#include "AtlasCore/span_reader.h"
#include "AtlasUE.h"
#include "entitydef/entitydef_api.h"

#include "gen/Account.gen.h"
#include "gen/Avatar.gen.h"
#include "gen/StressAvatar.gen.h"

namespace
{
struct FCapturedRpc
{
	atlas::EntityId Id = 0;
	uint32 RpcId = 0;
	std::vector<uint8_t> Args;
	bool Base = false;
};

class FCaptureSender : public atlas::RpcSender
{
public:
	std::vector<FCapturedRpc> Calls;

	void SendBaseRpc(atlas::EntityId Id, uint32_t RpcId, const uint8_t* Args, int32_t Len) override
	{
		FCapturedRpc r;
		r.Id = Id;
		r.RpcId = RpcId;
		r.Args.assign(Args, Args + Len);
		r.Base = true;
		Calls.push_back(std::move(r));
	}
	void SendCellRpc(atlas::EntityId Id, uint32_t RpcId, const uint8_t* Args, int32_t Len) override
	{
		FCapturedRpc r;
		r.Id = Id;
		r.RpcId = RpcId;
		r.Args.assign(Args, Args + Len);
		r.Base = false;
		Calls.push_back(std::move(r));
	}
};
}  // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAtlasGenRpcStubsTest,
	"Atlas.Codegen.RpcStubsPackArgs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAtlasGenRpcStubsTest::RunTest(const FString&)
{
	FCaptureSender Sender;

	// Account.SelectAvatar(1) — base RPC, single int32 arg.
	{
		atlas::mvp::Account Account(99, atlas::mvp::Account::kTypeId);
		Account.SetRpcSender(&Sender);
		Account.SelectAvatar(1);

		if (!TestEqual(TEXT("one base rpc captured"),
				static_cast<int32>(Sender.Calls.size()), 1)) return false;
		const auto& Call = Sender.Calls[0];
		TestTrue(TEXT("routed to base"), Call.Base);
		TestEqual(TEXT("rpc_id matches encoded constant"), Call.RpcId,
			atlas::mvp::Account::kRpcId_SelectAvatar);
		TestEqual(TEXT("entity id forwarded"), Call.Id, static_cast<atlas::EntityId>(99));
		if (TestEqual(TEXT("args = sizeof(int32)"), static_cast<int32>(Call.Args.size()), 4))
		{
			int32_t Decoded = 0;
			std::memcpy(&Decoded, Call.Args.data(), 4);
			TestEqual(TEXT("arg value 1"), Decoded, 1);
		}
	}

	Sender.Calls.clear();

	// Avatar.ReportPos(vec3, vec3) — cell RPC, six floats.
	{
		atlas::mvp::Avatar Av(42, atlas::mvp::Avatar::kTypeId);
		Av.SetRpcSender(&Sender);
		Av.ReportPos(atlas::Vec3{1.0f, 2.0f, 3.0f}, atlas::Vec3{0.0f, 1.0f, 0.0f});

		if (!TestEqual(TEXT("one cell rpc captured"),
				static_cast<int32>(Sender.Calls.size()), 1)) return false;
		const auto& Call = Sender.Calls[0];
		TestFalse(TEXT("routed to cell"), Call.Base);
		TestEqual(TEXT("rpc_id matches"), Call.RpcId, atlas::mvp::Avatar::kRpcId_ReportPos);
		TestEqual(TEXT("args = 6 floats"), static_cast<int32>(Call.Args.size()), 24);
		float Vals[6] = {};
		std::memcpy(Vals, Call.Args.data(), sizeof(Vals));
		TestEqual(TEXT("pos.x"), Vals[0], 1.0f);
		TestEqual(TEXT("pos.y"), Vals[1], 2.0f);
		TestEqual(TEXT("pos.z"), Vals[2], 3.0f);
		TestEqual(TEXT("dir.x"), Vals[3], 0.0f);
		TestEqual(TEXT("dir.y"), Vals[4], 1.0f);
		TestEqual(TEXT("dir.z"), Vals[5], 0.0f);
	}

	// No sender → SelectAvatar silently no-ops (used by unit tests that
	// only verify decode without setting up a transport).
	{
		atlas::mvp::Account Solo(1, atlas::mvp::Account::kTypeId);
		Solo.SelectAvatar(5);  // must not crash
		TestTrue(TEXT("no-sender call survives"), true);
	}

	return true;
}

// Down-side: verify codegen's DispatchRpc decodes args and forks to the
// matching virtual. A game-side subclass captures the call so we can assert
// the values landed correctly.
namespace
{
class FCaptureAvatar : public atlas::mvp::Avatar
{
public:
	FCaptureAvatar(atlas::EntityId Id) : atlas::mvp::Avatar(Id, kTypeId) {}

	int ShowDamageCount = 0;
	int32_t LastDamage = 0;
	uint32_t LastDamageAttacker = 0;

	int OnDiedCount = 0;
	uint32_t LastDiedAttacker = 0;

	int OnProjectileFiredCount = 0;
	uint32_t LastShotId = 0;
	atlas::Vec3 LastOrigin{};
	atlas::Vec3 LastVelocity{};

	void ShowDamage(int32_t amount, uint32_t attackerId) override
	{
		++ShowDamageCount;
		LastDamage = amount;
		LastDamageAttacker = attackerId;
	}
	void OnDied(uint32_t attackerId) override
	{
		++OnDiedCount;
		LastDiedAttacker = attackerId;
	}
	void OnProjectileFired(uint32_t shotId, const atlas::Vec3& origin,
	                       const atlas::Vec3& velocity) override
	{
		++OnProjectileFiredCount;
		LastShotId = shotId;
		LastOrigin = origin;
		LastVelocity = velocity;
	}
};
}  // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAtlasGenDispatchRpcTest,
	"Atlas.Codegen.DispatchRpcRoutesArgs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAtlasGenDispatchRpcTest::RunTest(const FString&)
{
	FCaptureAvatar A(42);

	// ShowDamage(amount=25, attackerId=7)
	{
		std::vector<uint8_t> Buf;
		const int32_t Amount = 25;
		const uint32_t Attacker = 7;
		Buf.resize(sizeof(int32_t) + sizeof(uint32_t));
		std::memcpy(Buf.data(), &Amount, 4);
		std::memcpy(Buf.data() + 4, &Attacker, 4);
		atlas::SpanReader R(Buf.data(), Buf.size());
		TestTrue(TEXT("dispatch ShowDamage"),
			A.DispatchRpc(atlas::mvp::Avatar::kRpcId_ShowDamage, 0, R));
		TestEqual(TEXT("ShowDamage fired once"), A.ShowDamageCount, 1);
		TestEqual(TEXT("damage value"), A.LastDamage, 25);
		TestEqual(TEXT("damage attacker"), A.LastDamageAttacker, 7u);
	}

	// OnProjectileFired — three-arg case with vector3 mixed in.
	{
		std::vector<uint8_t> Buf;
		const uint32_t ShotId = 99;
		Buf.resize(4 + 12 + 12);
		std::memcpy(Buf.data(), &ShotId, 4);
		float O[3] = {1.0f, 2.0f, 3.0f};
		float V[3] = {0.0f, 0.0f, 5.0f};
		std::memcpy(Buf.data() + 4, O, 12);
		std::memcpy(Buf.data() + 16, V, 12);
		atlas::SpanReader R(Buf.data(), Buf.size());
		TestTrue(TEXT("dispatch OnProjectileFired"),
			A.DispatchRpc(atlas::mvp::Avatar::kRpcId_OnProjectileFired, 0, R));
		TestEqual(TEXT("shotId"), A.LastShotId, 99u);
		TestEqual(TEXT("origin.x"), A.LastOrigin.x, 1.0f);
		TestEqual(TEXT("origin.z"), A.LastOrigin.z, 3.0f);
		TestEqual(TEXT("velocity.z"), A.LastVelocity.z, 5.0f);
	}

	// Unknown rpc_id — DispatchRpc returns false, no virtual called.
	{
		atlas::SpanReader R(nullptr, 0);
		TestFalse(TEXT("unknown rpc_id ignored"),
			A.DispatchRpc(0xDEADBEEF, 0, R));
		TestEqual(TEXT("no extra ShowDamage fired"), A.ShowDamageCount, 1);
	}

	// Truncated payload — return false, virtual NOT called.
	{
		std::vector<uint8_t> Buf(2);  // only 2 bytes for int32 amount → fail
		atlas::SpanReader R(Buf.data(), Buf.size());
		TestFalse(TEXT("truncated ShowDamage rejected"),
			A.DispatchRpc(atlas::mvp::Avatar::kRpcId_ShowDamage, 0, R));
		TestEqual(TEXT("ShowDamage NOT fired"), A.ShowDamageCount, 1);
	}

	// Bare ClientEntity (no codegen override) returns false always.
	{
		atlas::ClientEntity Bare(1, 0);
		atlas::SpanReader R(nullptr, 0);
		TestFalse(TEXT("base DispatchRpc default"), Bare.DispatchRpc(0, 0, R));
	}

	return true;
}

// Property-change hooks: feed a real delta through ApplyDelta and assert
// that the codegen's snapshot/diff fires the matching virtuals exactly once
// per actually-changed prop.
namespace
{
class FCaptureAvatarProps : public atlas::mvp::Avatar
{
public:
	FCaptureAvatarProps(atlas::EntityId Id) : atlas::mvp::Avatar(Id, kTypeId) {}

	int HpChangedCount = 0;
	int32_t HpOld = 0, HpNew = 0;
	int LevelChangedCount = 0;
	int32_t LevelOld = 0, LevelNew = 0;
	int SecretChangedCount = 0;
	std::string SecretOld, SecretNew;

	void OnHpChanged(int32_t Old, int32_t New) override
	{
		++HpChangedCount; HpOld = Old; HpNew = New;
	}
	void OnLevelChanged(int32_t Old, int32_t New) override
	{
		++LevelChangedCount; LevelOld = Old; LevelNew = New;
	}
	void OnSecretChanged(const std::string& Old, const std::string& New) override
	{
		++SecretChangedCount; SecretOld = Old; SecretNew = New;
	}
};

template <typename T>
void AppendProp(std::vector<uint8_t>& buf, T v)
{
	const auto offset = buf.size();
	buf.resize(offset + sizeof(T));
	std::memcpy(buf.data() + offset, &v, sizeof(T));
}
}  // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAtlasGenPropertyChangeTest,
	"Atlas.Codegen.PropertyChangeHooks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAtlasGenPropertyChangeTest::RunTest(const FString&)
{
	AtlasEdrContext* Ctx = FAtlasUEModule::GetEdrContext();
	if (!TestNotNull(TEXT("edr context loaded"), Ctx)) return false;
	const AtlasEdrEntity* AvatarDesc = AtlasEdrFindEntityByName(Ctx, "Avatar");
	if (!TestNotNull(TEXT("avatar descriptor"), AvatarDesc)) return false;

	FCaptureAvatarProps A(42);
	A.BindDescriptor(AvatarDesc, Ctx);

	// First delta: Hp=100, Level=5. Both transition 0→new, both hooks fire.
	{
		std::vector<uint8_t> Buf;
		AppendProp<uint8_t>(Buf, 0x01);                // sectionMask scalars
		AppendProp<uint8_t>(Buf, (1 << 0) | (1 << 4)); // Hp + Level bits
		AppendProp<int32_t>(Buf, 100);
		AppendProp<int32_t>(Buf, 5);
		atlas::SpanReader R(Buf.data(), Buf.size());
		TestTrue(TEXT("apply delta 1"), A.ApplyDelta(R));
		TestEqual(TEXT("hp fired once"), A.HpChangedCount, 1);
		TestEqual(TEXT("hp old=0"), A.HpOld, 0);
		TestEqual(TEXT("hp new=100"), A.HpNew, 100);
		TestEqual(TEXT("level fired once"), A.LevelChangedCount, 1);
		TestEqual(TEXT("level old=0"), A.LevelOld, 0);
		TestEqual(TEXT("level new=5"), A.LevelNew, 5);
		TestEqual(TEXT("secret untouched"), A.SecretChangedCount, 0);
	}

	// Second delta: Hp=80 (changes), Level still 5 in mask but value same → no fire.
	{
		std::vector<uint8_t> Buf;
		AppendProp<uint8_t>(Buf, 0x01);
		AppendProp<uint8_t>(Buf, (1 << 0) | (1 << 4));
		AppendProp<int32_t>(Buf, 80);
		AppendProp<int32_t>(Buf, 5);
		atlas::SpanReader R(Buf.data(), Buf.size());
		TestTrue(TEXT("apply delta 2"), A.ApplyDelta(R));
		TestEqual(TEXT("hp fired again"), A.HpChangedCount, 2);
		TestEqual(TEXT("hp old=100"), A.HpOld, 100);
		TestEqual(TEXT("hp new=80"), A.HpNew, 80);
		TestEqual(TEXT("level NOT fired (value same)"), A.LevelChangedCount, 1);
	}

	// String property: secret 空→"alpha" triggers hook.
	{
		std::vector<uint8_t> Buf;
		AppendProp<uint8_t>(Buf, 0x01);
		AppendProp<uint8_t>(Buf, 1 << 3);  // Secret bit
		AppendProp<uint8_t>(Buf, 5);       // PackedUInt32 len
		for (char c : std::string("alpha")) AppendProp<uint8_t>(Buf, static_cast<uint8_t>(c));
		atlas::SpanReader R(Buf.data(), Buf.size());
		TestTrue(TEXT("apply delta 3"), A.ApplyDelta(R));
		TestEqual(TEXT("secret fired"), A.SecretChangedCount, 1);
		TestEqual(TEXT("secret old empty"), FString(A.SecretOld.c_str()), FString(""));
		TestEqual(TEXT("secret new alpha"), FString(A.SecretNew.c_str()), FString("alpha"));
	}

	return true;
}

// Container + struct RPC args round-trip through the codegen pack/unpack.
namespace
{
class FCaptureStress : public atlas::mvp::StressAvatar
{
public:
	FCaptureStress(atlas::EntityId Id) : atlas::mvp::StressAvatar(Id, kTypeId) {}

	int OnWeaponBrokenCount = 0;
	atlas::mvp::StressWeapon LastWeapon{};

	int OnScoresSnapshotCount = 0;
	std::vector<int32_t> LastScores;

	void OnWeaponBroken(const atlas::mvp::StressWeapon& w) override
	{
		++OnWeaponBrokenCount;
		LastWeapon = w;
	}
	void OnScoresSnapshot(const std::vector<int32_t>& scores) override
	{
		++OnScoresSnapshotCount;
		LastScores = scores;
	}
};
}  // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAtlasGenContainerRpcTest,
	"Atlas.Codegen.ContainerAndStructRpcArgs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAtlasGenContainerRpcTest::RunTest(const FString&)
{
	FCaptureSender Sender;
	FCaptureStress S(77);
	S.SetRpcSender(&Sender);

	// Upstream list[int32]: SubmitScores([10, 20, 30]) — wire = [u16 count][int32]*
	{
		Sender.Calls.clear();
		S.SubmitScores({10, 20, 30});
		if (!TestEqual(TEXT("captured 1"), static_cast<int32>(Sender.Calls.size()), 1)) return false;
		const auto& c = Sender.Calls[0];
		TestEqual(TEXT("cell rpc"), c.Base, false);
		TestEqual(TEXT("submit rpc id"), c.RpcId, atlas::mvp::StressAvatar::kRpcId_SubmitScores);
		if (TestEqual(TEXT("submit bytes"), static_cast<int32>(c.Args.size()), 2 + 4 * 3))
		{
			uint16_t count = 0;
			std::memcpy(&count, c.Args.data(), 2);
			TestEqual(TEXT("count=3"), count, static_cast<uint16_t>(3));
			int32_t vals[3];
			std::memcpy(vals, c.Args.data() + 2, 12);
			TestEqual(TEXT("vals[0]"), vals[0], 10);
			TestEqual(TEXT("vals[2]"), vals[2], 30);
		}
	}

	// Upstream struct: EquipWeapon — wire = [int32 id][u16 sharp][u8 bound]
	{
		Sender.Calls.clear();
		atlas::mvp::StressWeapon w{7, 200, true};
		S.EquipWeapon(w);
		if (!TestEqual(TEXT("captured 1"), static_cast<int32>(Sender.Calls.size()), 1)) return false;
		const auto& c = Sender.Calls[0];
		if (TestEqual(TEXT("equip bytes"), static_cast<int32>(c.Args.size()), 4 + 2 + 1))
		{
			int32_t id = 0;
			uint16_t sharp = 0;
			uint8_t bound = 0;
			std::memcpy(&id, c.Args.data(), 4);
			std::memcpy(&sharp, c.Args.data() + 4, 2);
			std::memcpy(&bound, c.Args.data() + 6, 1);
			TestEqual(TEXT("id=7"), id, 7);
			TestEqual(TEXT("sharp=200"), sharp, static_cast<uint16_t>(200));
			TestEqual(TEXT("bound=1"), bound, static_cast<uint8_t>(1));
		}
	}

	// Upstream list[struct]: ApplyBuffs — wire = [u16 count][struct]*
	{
		Sender.Calls.clear();
		std::vector<atlas::mvp::StressBuff> buffs{{1, 2, 300u}};
		S.ApplyBuffs(buffs);
		if (!TestEqual(TEXT("captured 1"), static_cast<int32>(Sender.Calls.size()), 1)) return false;
		const auto& c = Sender.Calls[0];
		if (TestEqual(TEXT("buffs bytes"), static_cast<int32>(c.Args.size()), 2 + 4 * 3))
		{
			uint16_t count = 0;
			int32_t kind = 0, stacks = 0;
			uint32_t dur = 0;
			std::memcpy(&count, c.Args.data(), 2);
			std::memcpy(&kind, c.Args.data() + 2, 4);
			std::memcpy(&stacks, c.Args.data() + 6, 4);
			std::memcpy(&dur, c.Args.data() + 10, 4);
			TestEqual(TEXT("count=1"), count, static_cast<uint16_t>(1));
			TestEqual(TEXT("kind=1"), kind, 1);
			TestEqual(TEXT("stacks=2"), stacks, 2);
			TestEqual(TEXT("dur=300"), dur, 300u);
		}
	}

	// Downstream struct: OnWeaponBroken
	{
		std::vector<uint8_t> Buf(4 + 2 + 1);
		int32_t id = 42; uint16_t sharp = 99; uint8_t bound = 0;
		std::memcpy(Buf.data(), &id, 4);
		std::memcpy(Buf.data() + 4, &sharp, 2);
		std::memcpy(Buf.data() + 6, &bound, 1);
		atlas::SpanReader R(Buf.data(), Buf.size());
		TestTrue(TEXT("dispatch OnWeaponBroken"),
			S.DispatchRpc(atlas::mvp::StressAvatar::kRpcId_OnWeaponBroken, 0, R));
		TestEqual(TEXT("ow fired"), S.OnWeaponBrokenCount, 1);
		TestEqual(TEXT("ow id"), S.LastWeapon.id, 42);
		TestEqual(TEXT("ow sharp"), S.LastWeapon.sharpness, static_cast<uint16_t>(99));
		TestEqual(TEXT("ow bound"), S.LastWeapon.bound, false);
	}

	// Downstream list[int32]: OnScoresSnapshot
	{
		std::vector<uint8_t> Buf;
		uint16_t cnt = 4;
		Buf.resize(2);
		std::memcpy(Buf.data(), &cnt, 2);
		for (int32_t v : {5, 6, 7, 8})
		{
			const auto off = Buf.size();
			Buf.resize(off + 4);
			std::memcpy(Buf.data() + off, &v, 4);
		}
		atlas::SpanReader R(Buf.data(), Buf.size());
		TestTrue(TEXT("dispatch OnScoresSnapshot"),
			S.DispatchRpc(atlas::mvp::StressAvatar::kRpcId_OnScoresSnapshot, 0, R));
		TestEqual(TEXT("os fired"), S.OnScoresSnapshotCount, 1);
		if (TestEqual(TEXT("os size"), static_cast<int32>(S.LastScores.size()), 4))
		{
			TestEqual(TEXT("os[0]"), S.LastScores[0], 5);
			TestEqual(TEXT("os[3]"), S.LastScores[3], 8);
		}
	}

	return true;
}

#endif  // WITH_DEV_AUTOMATION_TESTS
