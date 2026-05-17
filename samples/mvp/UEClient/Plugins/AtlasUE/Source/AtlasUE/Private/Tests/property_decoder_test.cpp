#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Interfaces/IPluginManager.h"

#if WITH_DEV_AUTOMATION_TESTS

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "AtlasCore/client_entity.h"
#include "AtlasCore/property_value.h"
#include "AtlasCore/span_reader.h"
#include "entitydef/entitydef_api.h"

namespace
{
template <typename T>
void AppendDec(std::vector<uint8_t>& buf, T v)
{
	const auto offset = buf.size();
	buf.resize(offset + sizeof(T));
	std::memcpy(buf.data() + offset, &v, sizeof(T));
}

void AppendString(std::vector<uint8_t>& buf, const char* s)
{
	uint32_t len = 0;
	while (s[len] != '\0') ++len;
	check(len < 0xFE);  // tests use short strings; long path covered in apply-delta tests
	AppendDec<uint8_t>(buf, static_cast<uint8_t>(len));
	for (uint32_t i = 0; i < len; ++i) AppendDec<uint8_t>(buf, static_cast<uint8_t>(s[i]));
}

// Loads the ClientSample ATDF (staged by build_mvp_ue.py) into a fresh
// AtlasEdrContext. Tests own this context so production stays on the
// MVP descriptor set.
AtlasEdrContext* LoadTestRegistry()
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

// Container-section helpers. Property bit indices follow StressAvatar's
// client-visible declaration order: hp=0, mainWeapon=1, scores=2,
// resists=3, combos=4, loadouts=5, buffs=6, spellSlots=7.
constexpr uint8_t kScoresBit = 1 << 2;
constexpr uint8_t kResistsBit = 1 << 3;
constexpr uint8_t kCombosBit = 1 << 4;
constexpr uint8_t kLoadoutsBit = 1 << 5;
constexpr uint8_t kBuffsBit = 1 << 6;
constexpr uint8_t kMainWeaponBit = 1 << 1;

enum : uint8_t {
	kOpSet = 0,
	kOpListSplice = 1,
	kOpDictSet = 2,
	kOpDictErase = 3,
	kOpClear = 4,
	kOpStructFieldSet = 5,
};
}  // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAtlasPropertyDecoderTest,
	"Atlas.PropertyDecoder.StressMatrix",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAtlasPropertyDecoderTest::RunTest(const FString&)
{
	AtlasEdrContext* Ctx = LoadTestRegistry();
	if (!TestNotNull(TEXT("test atdf loaded"), Ctx)) return false;

	const AtlasEdrEntity* Desc = AtlasEdrFindEntityByName(Ctx, "StressAvatar");
	if (!TestNotNull(TEXT("StressAvatar descriptor found"), Desc))
	{
		AtlasEdrDestroy(Ctx);
		return false;
	}

	// Scalar struct: mainWeapon = (id=42, sharpness=300, bound=true).
	{
		atlas::ClientEntity Entity(1, 3);
		Entity.BindDescriptor(Desc, Ctx);
		std::vector<uint8_t> Buf;
		AppendDec<uint8_t>(Buf, 0x01);  // sectionMask: scalars
		AppendDec<uint8_t>(Buf, kMainWeaponBit);
		AppendDec<int32_t>(Buf, 42);
		AppendDec<uint16_t>(Buf, 300);
		AppendDec<uint8_t>(Buf, 1);
		atlas::SpanReader R(Buf.data(), Buf.size());
		TestTrue(TEXT("mainWeapon scalar struct decode"), Entity.ApplyDelta(R));
		const auto& Slot = Entity.Properties()[1];
		auto* SV = std::holds_alternative<std::unique_ptr<atlas::StructValue>>(Slot)
			? std::get<std::unique_ptr<atlas::StructValue>>(Slot).get() : nullptr;
		if (TestNotNull(TEXT("mainWeapon is StructValue"), SV))
		{
			TestEqual(TEXT("mainWeapon.id"), std::get<int32_t>(SV->fields[0]), 42);
			TestEqual(TEXT("mainWeapon.sharpness"), std::get<uint16_t>(SV->fields[1]),
				static_cast<uint16_t>(300));
			TestEqual(TEXT("mainWeapon.bound"), std::get<bool>(SV->fields[2]), true);
		}
	}

	// list[int32] scores: ListSplice insert [10,20,30], then Set slot 1 → 99,
	// then Clear all in three separate deltas to cover each op kind.
	{
		atlas::ClientEntity Entity(2, 3);
		Entity.BindDescriptor(Desc, Ctx);

		{
			std::vector<uint8_t> Buf;
			AppendDec<uint8_t>(Buf, 0x02);  // containers
			AppendDec<uint8_t>(Buf, kScoresBit);
			AppendDec<uint16_t>(Buf, 1);     // op_count
			AppendDec<uint8_t>(Buf, kOpListSplice);
			AppendDec<uint16_t>(Buf, 0);     // start
			AppendDec<uint16_t>(Buf, 0);     // end
			AppendDec<uint16_t>(Buf, 3);     // vcount
			AppendDec<int32_t>(Buf, 10);
			AppendDec<int32_t>(Buf, 20);
			AppendDec<int32_t>(Buf, 30);
			atlas::SpanReader R(Buf.data(), Buf.size());
			TestTrue(TEXT("scores splice insert"), Entity.ApplyDelta(R));
		}
		auto* LV = std::get<std::unique_ptr<atlas::ListValue>>(Entity.Properties()[2]).get();
		if (TestNotNull(TEXT("scores is ListValue"), LV))
		{
			TestEqual(TEXT("scores size after splice"), static_cast<int32>(LV->items.size()), 3);
			TestEqual(TEXT("scores[0]"), std::get<int32_t>(LV->items[0]), 10);
			TestEqual(TEXT("scores[1]"), std::get<int32_t>(LV->items[1]), 20);
			TestEqual(TEXT("scores[2]"), std::get<int32_t>(LV->items[2]), 30);
		}

		{
			std::vector<uint8_t> Buf;
			AppendDec<uint8_t>(Buf, 0x02);
			AppendDec<uint8_t>(Buf, kScoresBit);
			AppendDec<uint16_t>(Buf, 1);
			AppendDec<uint8_t>(Buf, kOpSet);
			AppendDec<uint16_t>(Buf, 1);
			AppendDec<int32_t>(Buf, 99);
			atlas::SpanReader R(Buf.data(), Buf.size());
			TestTrue(TEXT("scores Set slot 1"), Entity.ApplyDelta(R));
		}
		LV = std::get<std::unique_ptr<atlas::ListValue>>(Entity.Properties()[2]).get();
		TestEqual(TEXT("scores[1] after Set"), std::get<int32_t>(LV->items[1]), 99);

		{
			std::vector<uint8_t> Buf;
			AppendDec<uint8_t>(Buf, 0x02);
			AppendDec<uint8_t>(Buf, kScoresBit);
			AppendDec<uint16_t>(Buf, 1);
			AppendDec<uint8_t>(Buf, kOpClear);
			atlas::SpanReader R(Buf.data(), Buf.size());
			TestTrue(TEXT("scores Clear"), Entity.ApplyDelta(R));
		}
		LV = std::get<std::unique_ptr<atlas::ListValue>>(Entity.Properties()[2]).get();
		TestEqual(TEXT("scores empty after Clear"), static_cast<int32>(LV->items.size()), 0);
	}

	// dict[string,int32] resists: DictSet two keys, then DictErase one.
	{
		atlas::ClientEntity Entity(3, 3);
		Entity.BindDescriptor(Desc, Ctx);
		{
			std::vector<uint8_t> Buf;
			AppendDec<uint8_t>(Buf, 0x02);
			AppendDec<uint8_t>(Buf, kResistsBit);
			AppendDec<uint16_t>(Buf, 2);  // 2 ops
			AppendDec<uint8_t>(Buf, kOpDictSet);
			AppendString(Buf, "fire");
			AppendDec<int32_t>(Buf, 100);
			AppendDec<uint8_t>(Buf, kOpDictSet);
			AppendString(Buf, "ice");
			AppendDec<int32_t>(Buf, 50);
			atlas::SpanReader R(Buf.data(), Buf.size());
			TestTrue(TEXT("resists DictSet x2"), Entity.ApplyDelta(R));
		}
		auto* DV = std::get<std::unique_ptr<atlas::DictValue>>(Entity.Properties()[3]).get();
		if (TestNotNull(TEXT("resists is DictValue"), DV))
		{
			TestEqual(TEXT("resists entry count"), static_cast<int32>(DV->entries.size()), 2);
		}

		{
			std::vector<uint8_t> Buf;
			AppendDec<uint8_t>(Buf, 0x02);
			AppendDec<uint8_t>(Buf, kResistsBit);
			AppendDec<uint16_t>(Buf, 1);
			AppendDec<uint8_t>(Buf, kOpDictErase);
			AppendString(Buf, "fire");
			atlas::SpanReader R(Buf.data(), Buf.size());
			TestTrue(TEXT("resists DictErase fire"), Entity.ApplyDelta(R));
		}
		DV = std::get<std::unique_ptr<atlas::DictValue>>(Entity.Properties()[3]).get();
		TestEqual(TEXT("resists after erase"), static_cast<int32>(DV->entries.size()), 1);
		TestEqual(TEXT("remaining key"),
			FString(std::get<std::string>(DV->entries[0].first).c_str()), FString("ice"));
	}

	// list[StressBuff] buffs: insert one struct, then StructFieldSet to bump
	// stacks (field index 1) to 5.
	{
		atlas::ClientEntity Entity(4, 3);
		Entity.BindDescriptor(Desc, Ctx);
		std::vector<uint8_t> Buf;
		AppendDec<uint8_t>(Buf, 0x02);
		AppendDec<uint8_t>(Buf, kBuffsBit);
		AppendDec<uint16_t>(Buf, 2);
		AppendDec<uint8_t>(Buf, kOpListSplice);
		AppendDec<uint16_t>(Buf, 0);
		AppendDec<uint16_t>(Buf, 0);
		AppendDec<uint16_t>(Buf, 1);
		AppendDec<int32_t>(Buf, 10);  // kind
		AppendDec<int32_t>(Buf, 1);   // stacks
		AppendDec<uint32_t>(Buf, 500u);  // durationMs
		AppendDec<uint8_t>(Buf, kOpStructFieldSet);
		AppendDec<uint16_t>(Buf, 0);  // slot
		AppendDec<uint8_t>(Buf, 1);   // field id = stacks
		AppendDec<int32_t>(Buf, 5);
		atlas::SpanReader R(Buf.data(), Buf.size());
		TestTrue(TEXT("buffs splice + struct field set"), Entity.ApplyDelta(R));
		auto* LV = std::get<std::unique_ptr<atlas::ListValue>>(Entity.Properties()[6]).get();
		if (TestNotNull(TEXT("buffs is ListValue"), LV) && LV->items.size() == 1)
		{
			auto* SV = std::get<std::unique_ptr<atlas::StructValue>>(LV->items[0]).get();
			if (TestNotNull(TEXT("buffs[0] is StructValue"), SV))
			{
				TestEqual(TEXT("buffs[0].kind"), std::get<int32_t>(SV->fields[0]), 10);
				TestEqual(TEXT("buffs[0].stacks after field set"),
					std::get<int32_t>(SV->fields[1]), 5);
				TestEqual(TEXT("buffs[0].durationMs"),
					std::get<uint32_t>(SV->fields[2]), 500u);
			}
		}
	}

	// list[list[int32]] combos: top-level Splice inserts one inner list with
	// two ints, then a child-dirty path mutates inner slot 0 via its own Set.
	{
		atlas::ClientEntity Entity(5, 3);
		Entity.BindDescriptor(Desc, Ctx);
		{
			std::vector<uint8_t> Buf;
			AppendDec<uint8_t>(Buf, 0x02);
			AppendDec<uint8_t>(Buf, kCombosBit);
			AppendDec<uint16_t>(Buf, 1);
			AppendDec<uint8_t>(Buf, kOpListSplice);
			AppendDec<uint16_t>(Buf, 0);
			AppendDec<uint16_t>(Buf, 0);
			AppendDec<uint16_t>(Buf, 1);
			// One inner list[int32] in integral form: [u16 count][int]*
			AppendDec<uint16_t>(Buf, 2);
			AppendDec<int32_t>(Buf, 7);
			AppendDec<int32_t>(Buf, 8);
			AppendDec<uint8_t>(Buf, 0);  // child-dirty count (PackedUInt32 < 0xFE)
			atlas::SpanReader R(Buf.data(), Buf.size());
			TestTrue(TEXT("combos splice insert inner list"), Entity.ApplyDelta(R));
		}
		auto* Outer = std::get<std::unique_ptr<atlas::ListValue>>(Entity.Properties()[4]).get();
		if (TestNotNull(TEXT("combos is ListValue"), Outer) && Outer->items.size() == 1)
		{
			auto* Inner = std::get<std::unique_ptr<atlas::ListValue>>(Outer->items[0]).get();
			if (TestNotNull(TEXT("combos[0] is inner ListValue"), Inner))
			{
				TestEqual(TEXT("inner size"), static_cast<int32>(Inner->items.size()), 2);
				TestEqual(TEXT("inner[0]"), std::get<int32_t>(Inner->items[0]), 7);
				TestEqual(TEXT("inner[1]"), std::get<int32_t>(Inner->items[1]), 8);
			}
		}

		{
			std::vector<uint8_t> Buf;
			AppendDec<uint8_t>(Buf, 0x02);
			AppendDec<uint8_t>(Buf, kCombosBit);
			AppendDec<uint16_t>(Buf, 0);  // 0 top-level ops
			AppendDec<uint8_t>(Buf, 1);   // child-dirty count = 1
			AppendDec<uint16_t>(Buf, 0);  // slot 0
			// Recurse: child ops on inner list[int32]
			AppendDec<uint16_t>(Buf, 1);  // 1 op
			AppendDec<uint8_t>(Buf, kOpSet);
			AppendDec<uint16_t>(Buf, 0);  // slot
			AppendDec<int32_t>(Buf, 77);
			atlas::SpanReader R(Buf.data(), Buf.size());
			TestTrue(TEXT("combos child-dirty mutates inner"), Entity.ApplyDelta(R));
		}
		Outer = std::get<std::unique_ptr<atlas::ListValue>>(Entity.Properties()[4]).get();
		auto* Inner2 = std::get<std::unique_ptr<atlas::ListValue>>(Outer->items[0]).get();
		TestEqual(TEXT("inner[0] after child dirty"), std::get<int32_t>(Inner2->items[0]), 77);
	}

	// dict[string, list[int]] loadouts: DictSet "primary"=[1,2], then
	// child-dirty rewrites inner slot 0 to 99.
	{
		atlas::ClientEntity Entity(6, 3);
		Entity.BindDescriptor(Desc, Ctx);
		{
			std::vector<uint8_t> Buf;
			AppendDec<uint8_t>(Buf, 0x02);
			AppendDec<uint8_t>(Buf, kLoadoutsBit);
			AppendDec<uint16_t>(Buf, 1);
			AppendDec<uint8_t>(Buf, kOpDictSet);
			AppendString(Buf, "primary");
			AppendDec<uint16_t>(Buf, 2);  // integral list count
			AppendDec<int32_t>(Buf, 1);
			AppendDec<int32_t>(Buf, 2);
			AppendDec<uint8_t>(Buf, 0);   // child-dirty count
			atlas::SpanReader R(Buf.data(), Buf.size());
			TestTrue(TEXT("loadouts DictSet primary=[1,2]"), Entity.ApplyDelta(R));
		}
		auto* DV = std::get<std::unique_ptr<atlas::DictValue>>(Entity.Properties()[5]).get();
		if (TestNotNull(TEXT("loadouts is DictValue"), DV) && DV->entries.size() == 1)
		{
			auto* Inner = std::get<std::unique_ptr<atlas::ListValue>>(DV->entries[0].second).get();
			TestEqual(TEXT("loadouts primary size"), static_cast<int32>(Inner->items.size()), 2);
		}

		{
			std::vector<uint8_t> Buf;
			AppendDec<uint8_t>(Buf, 0x02);
			AppendDec<uint8_t>(Buf, kLoadoutsBit);
			AppendDec<uint16_t>(Buf, 0);  // 0 top-level
			AppendDec<uint8_t>(Buf, 1);   // child-dirty count
			AppendString(Buf, "primary");
			AppendDec<uint16_t>(Buf, 1);  // 1 inner op
			AppendDec<uint8_t>(Buf, kOpSet);
			AppendDec<uint16_t>(Buf, 0);
			AppendDec<int32_t>(Buf, 99);
			atlas::SpanReader R(Buf.data(), Buf.size());
			TestTrue(TEXT("loadouts child-dirty mutates inner"), Entity.ApplyDelta(R));
		}
		DV = std::get<std::unique_ptr<atlas::DictValue>>(Entity.Properties()[5]).get();
		auto* Inner = std::get<std::unique_ptr<atlas::ListValue>>(DV->entries[0].second).get();
		TestEqual(TEXT("loadouts primary[0] after child"), std::get<int32_t>(Inner->items[0]), 99);
	}

	// Truncated: half a struct field — ApplyDelta returns false.
	{
		atlas::ClientEntity Entity(7, 3);
		Entity.BindDescriptor(Desc, Ctx);
		std::vector<uint8_t> Buf;
		AppendDec<uint8_t>(Buf, 0x01);
		AppendDec<uint8_t>(Buf, kMainWeaponBit);
		AppendDec<int32_t>(Buf, 1);
		AppendDec<uint8_t>(Buf, 0);  // truncated: missing uint16 sharpness + bool
		atlas::SpanReader R(Buf.data(), Buf.size());
		TestFalse(TEXT("truncated struct rejected"), Entity.ApplyDelta(R));
	}

	// Malformed op kind — unknown op byte rejected.
	{
		atlas::ClientEntity Entity(8, 3);
		Entity.BindDescriptor(Desc, Ctx);
		std::vector<uint8_t> Buf;
		AppendDec<uint8_t>(Buf, 0x02);
		AppendDec<uint8_t>(Buf, kScoresBit);
		AppendDec<uint16_t>(Buf, 1);
		AppendDec<uint8_t>(Buf, 99);  // not a valid OpKind
		atlas::SpanReader R(Buf.data(), Buf.size());
		TestFalse(TEXT("unknown op kind rejected"), Entity.ApplyDelta(R));
	}

	AtlasEdrDestroy(Ctx);
	return true;
}

#endif  // WITH_DEV_AUTOMATION_TESTS
