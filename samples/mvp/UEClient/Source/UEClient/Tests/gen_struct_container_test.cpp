#include "CoreMinimal.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "AtlasCore/client_entity.h"
#include "AtlasCore/span_reader.h"
#include "entitydef/entitydef_api.h"

#include "gen/StressAvatar.gen.h"

namespace
{
template <typename T>
void AppendStruct(std::vector<uint8_t>& buf, T v)
{
	const auto offset = buf.size();
	buf.resize(offset + sizeof(T));
	std::memcpy(buf.data() + offset, &v, sizeof(T));
}

void AppendStructString(std::vector<uint8_t>& buf, const char* s)
{
	uint32_t len = 0;
	while (s[len] != '\0') ++len;
	check(len < 0xFE);
	AppendStruct<uint8_t>(buf, static_cast<uint8_t>(len));
	for (uint32_t i = 0; i < len; ++i) AppendStruct<uint8_t>(buf, static_cast<uint8_t>(s[i]));
}

AtlasEdrContext* LoadStressRegistry()
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("AtlasUE"));
	if (!Plugin.IsValid()) return nullptr;
	const FString Path = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		Plugin->GetBaseDir(), TEXT("ThirdParty"), TEXT("AtlasEntityDef"),
		TEXT("entity_defs_test.bin")));
	AtlasEdrContext* Ctx = AtlasEdrCreate(ATLAS_EDR_ABI_VERSION);
	if (Ctx == nullptr) return nullptr;
	const auto AnsiPath = StringCast<ANSICHAR>(*Path);
	if (AtlasEdrLoadFromFile(Ctx, AnsiPath.Get()) != ATLAS_EDR_OK)
	{
		AtlasEdrDestroy(Ctx);
		return nullptr;
	}
	return Ctx;
}

constexpr uint8_t kMainWeaponBit = 1 << 1;
constexpr uint8_t kScoresBit = 1 << 2;
constexpr uint8_t kResistsBit = 1 << 3;
constexpr uint8_t kCombosBit = 1 << 4;
constexpr uint8_t kLoadoutsBit = 1 << 5;
constexpr uint8_t kBuffsBit = 1 << 6;

enum : uint8_t {
	kOpListSplice = 1,
	kOpDictSet = 2,
};
}  // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAtlasGenStructContainerTest,
	"Atlas.Codegen.StructAndContainerGetters",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAtlasGenStructContainerTest::RunTest(const FString&)
{
	AtlasEdrContext* Ctx = LoadStressRegistry();
	if (!TestNotNull(TEXT("test atdf loaded"), Ctx)) return false;
	const AtlasEdrEntity* Desc = AtlasEdrFindEntityByName(Ctx, "StressAvatar");
	if (!TestNotNull(TEXT("StressAvatar descriptor"), Desc))
	{
		AtlasEdrDestroy(Ctx);
		return false;
	}

	atlas::mvp::StressAvatar A(7, atlas::mvp::StressAvatar::kTypeId);
	A.BindDescriptor(Desc, Ctx);

	// mainWeapon = (id=99, sharpness=250, bound=true)
	{
		std::vector<uint8_t> Buf;
		AppendStruct<uint8_t>(Buf, 0x01);
		AppendStruct<uint8_t>(Buf, kMainWeaponBit);
		AppendStruct<int32_t>(Buf, 99);
		AppendStruct<uint16_t>(Buf, 250);
		AppendStruct<uint8_t>(Buf, 1);
		atlas::SpanReader R(Buf.data(), Buf.size());
		TestTrue(TEXT("apply mainWeapon"), A.ApplyDelta(R));
		auto w = A.MainWeapon();
		TestEqual(TEXT("mainWeapon.id"), w.id, 99);
		TestEqual(TEXT("mainWeapon.sharpness"), w.sharpness, static_cast<uint16_t>(250));
		TestEqual(TEXT("mainWeapon.bound"), w.bound, true);
	}

	// scores = [10, 20, 30]
	{
		std::vector<uint8_t> Buf;
		AppendStruct<uint8_t>(Buf, 0x02);
		AppendStruct<uint8_t>(Buf, kScoresBit);
		AppendStruct<uint16_t>(Buf, 1);
		AppendStruct<uint8_t>(Buf, kOpListSplice);
		AppendStruct<uint16_t>(Buf, 0);
		AppendStruct<uint16_t>(Buf, 0);
		AppendStruct<uint16_t>(Buf, 3);
		AppendStruct<int32_t>(Buf, 10);
		AppendStruct<int32_t>(Buf, 20);
		AppendStruct<int32_t>(Buf, 30);
		atlas::SpanReader R(Buf.data(), Buf.size());
		TestTrue(TEXT("apply scores"), A.ApplyDelta(R));
		auto s = A.Scores();
		if (TestEqual(TEXT("scores size"), static_cast<int32>(s.size()), 3))
		{
			TestEqual(TEXT("scores[0]"), s[0], 10);
			TestEqual(TEXT("scores[1]"), s[1], 20);
			TestEqual(TEXT("scores[2]"), s[2], 30);
		}
	}

	// resists = {fire: 100, ice: 50}
	{
		std::vector<uint8_t> Buf;
		AppendStruct<uint8_t>(Buf, 0x02);
		AppendStruct<uint8_t>(Buf, kResistsBit);
		AppendStruct<uint16_t>(Buf, 2);
		AppendStruct<uint8_t>(Buf, kOpDictSet);
		AppendStructString(Buf, "fire");
		AppendStruct<int32_t>(Buf, 100);
		AppendStruct<uint8_t>(Buf, kOpDictSet);
		AppendStructString(Buf, "ice");
		AppendStruct<int32_t>(Buf, 50);
		atlas::SpanReader R(Buf.data(), Buf.size());
		TestTrue(TEXT("apply resists"), A.ApplyDelta(R));
		auto r = A.Resists();
		if (TestEqual(TEXT("resists size"), static_cast<int32>(r.size()), 2))
		{
			TestEqual(TEXT("resists[0].key"), FString(r[0].first.c_str()), FString("fire"));
			TestEqual(TEXT("resists[0].value"), r[0].second, 100);
			TestEqual(TEXT("resists[1].key"), FString(r[1].first.c_str()), FString("ice"));
			TestEqual(TEXT("resists[1].value"), r[1].second, 50);
		}
	}

	// buffs = [{kind=10, stacks=3, durationMs=500}]
	{
		std::vector<uint8_t> Buf;
		AppendStruct<uint8_t>(Buf, 0x02);
		AppendStruct<uint8_t>(Buf, kBuffsBit);
		AppendStruct<uint16_t>(Buf, 1);
		AppendStruct<uint8_t>(Buf, kOpListSplice);
		AppendStruct<uint16_t>(Buf, 0);
		AppendStruct<uint16_t>(Buf, 0);
		AppendStruct<uint16_t>(Buf, 1);
		AppendStruct<int32_t>(Buf, 10);
		AppendStruct<int32_t>(Buf, 3);
		AppendStruct<uint32_t>(Buf, 500u);
		atlas::SpanReader R(Buf.data(), Buf.size());
		TestTrue(TEXT("apply buffs"), A.ApplyDelta(R));
		auto b = A.Buffs();
		if (TestEqual(TEXT("buffs size"), static_cast<int32>(b.size()), 1))
		{
			TestEqual(TEXT("buffs[0].kind"), b[0].kind, 10);
			TestEqual(TEXT("buffs[0].stacks"), b[0].stacks, 3);
			TestEqual(TEXT("buffs[0].durationMs"), b[0].durationMs, 500u);
		}
	}

	// combos = [[7, 8]] — nested list-in-list extract
	{
		std::vector<uint8_t> Buf;
		AppendStruct<uint8_t>(Buf, 0x02);
		AppendStruct<uint8_t>(Buf, kCombosBit);
		AppendStruct<uint16_t>(Buf, 1);
		AppendStruct<uint8_t>(Buf, kOpListSplice);
		AppendStruct<uint16_t>(Buf, 0);
		AppendStruct<uint16_t>(Buf, 0);
		AppendStruct<uint16_t>(Buf, 1);
		AppendStruct<uint16_t>(Buf, 2);  // inner integral list count
		AppendStruct<int32_t>(Buf, 7);
		AppendStruct<int32_t>(Buf, 8);
		AppendStruct<uint8_t>(Buf, 0);   // child-dirty count = 0
		atlas::SpanReader R(Buf.data(), Buf.size());
		TestTrue(TEXT("apply combos"), A.ApplyDelta(R));
		auto c = A.Combos();
		if (TestEqual(TEXT("combos size"), static_cast<int32>(c.size()), 1)
				&& TestEqual(TEXT("inner size"), static_cast<int32>(c[0].size()), 2))
		{
			TestEqual(TEXT("combos[0][0]"), c[0][0], 7);
			TestEqual(TEXT("combos[0][1]"), c[0][1], 8);
		}
	}

	// loadouts = {primary: [1, 2]} — dict-of-list nested extract
	{
		std::vector<uint8_t> Buf;
		AppendStruct<uint8_t>(Buf, 0x02);
		AppendStruct<uint8_t>(Buf, kLoadoutsBit);
		AppendStruct<uint16_t>(Buf, 1);
		AppendStruct<uint8_t>(Buf, kOpDictSet);
		AppendStructString(Buf, "primary");
		AppendStruct<uint16_t>(Buf, 2);  // inner integral list count
		AppendStruct<int32_t>(Buf, 1);
		AppendStruct<int32_t>(Buf, 2);
		AppendStruct<uint8_t>(Buf, 0);   // child-dirty count = 0
		atlas::SpanReader R(Buf.data(), Buf.size());
		TestTrue(TEXT("apply loadouts"), A.ApplyDelta(R));
		auto l = A.Loadouts();
		if (TestEqual(TEXT("loadouts size"), static_cast<int32>(l.size()), 1)
				&& TestEqual(TEXT("loadouts[0].value size"),
					static_cast<int32>(l[0].second.size()), 2))
		{
			TestEqual(TEXT("loadouts[0].key"), FString(l[0].first.c_str()),
				FString("primary"));
			TestEqual(TEXT("loadouts[0].value[0]"), l[0].second[0], 1);
			TestEqual(TEXT("loadouts[0].value[1]"), l[0].second[1], 2);
		}
	}

	AtlasEdrDestroy(Ctx);
	return true;
}

#endif  // WITH_DEV_AUTOMATION_TESTS
