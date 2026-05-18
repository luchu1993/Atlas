#include "CoreMinimal.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS

#include <cstdint>
#include <cstring>
#include <vector>

#include "AtlasCore/client_entity.h"
#include "AtlasCore/component_instance.h"
#include "AtlasCore/rpc_sender.h"
#include "AtlasCore/span_reader.h"
#include "entitydef/entitydef_api.h"

#include "gen/StressAvatar.gen.h"
#include "gen/StressLoadComponent.gen.h"

namespace
{
template <typename T>
void AppendComp(std::vector<uint8_t>& buf, T v)
{
	const auto offset = buf.size();
	buf.resize(offset + sizeof(T));
	std::memcpy(buf.data() + offset, &v, sizeof(T));
}

struct FCapturedRpc
{
	atlas::EntityId Id = 0;
	uint32 RpcId = 0;
	std::vector<uint8_t> Args;
	bool Base = false;
};

class FCaptureSenderForComp : public atlas::RpcSender
{
public:
	std::vector<FCapturedRpc> Calls;
	void SendBaseRpc(atlas::EntityId Id, uint32_t Rpc, const uint8_t* A, int32_t L) override
	{
		Calls.push_back({Id, Rpc, std::vector<uint8_t>(A, A + L), true});
	}
	void SendCellRpc(atlas::EntityId Id, uint32_t Rpc, const uint8_t* A, int32_t L) override
	{
		Calls.push_back({Id, Rpc, std::vector<uint8_t>(A, A + L), false});
	}
};

// Game-side StressLoadComponent override capturing downstream RPC.
class FCaptureLoad : public atlas::mvp::StressLoadComponent
{
public:
	using StressLoadComponent::StressLoadComponent;
	int OnAffixesCount = 0;
	std::vector<int32_t> LastAffixes;
	void OnAffixesUpdated(const std::vector<int32_t>& ids) override
	{
		++OnAffixesCount;
		LastAffixes = ids;
	}
};

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
}  // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAtlasGenComponentTest,
	"Atlas.Codegen.LogicComponent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAtlasGenComponentTest::RunTest(const FString&)
{
	AtlasEdrContext* Ctx = LoadStressRegistry();
	if (!TestNotNull(TEXT("test atdf"), Ctx)) return false;
	const AtlasEdrEntity* Desc = AtlasEdrFindEntityByName(Ctx, "StressAvatar");
	if (!TestNotNull(TEXT("stress avatar descriptor"), Desc))
	{
		AtlasEdrDestroy(Ctx);
		return false;
	}

	// Factory swap must precede the first delta that lazily instantiates the slot.
	atlas::mvp::StressAvatar Entity(101, atlas::mvp::StressAvatar::kTypeId);
	Entity.RegisterComponentFactory(1,
		[](const AtlasEdrComponent* D, atlas::ClientEntity* O, uint8_t S) {
			return std::make_unique<FCaptureLoad>(D, O, S);
		});
	Entity.BindDescriptor(Desc, Ctx);

	FCaptureSenderForComp Sender;
	Entity.SetRpcSender(&Sender);

	// Wire: [u8 mask=0x04][PackedU32 slots=1][u8 slot=1]
	//       [u8 comp_mask=0x01][u8 flags=1<<0][i32 7]  // ExtraHp = 7
	{
		std::vector<uint8_t> Buf;
		AppendComp<uint8_t>(Buf, 0x04);  // entity sectionMask = component section only
		AppendComp<uint8_t>(Buf, 1);     // PackedUInt32 < 0xFE → single byte
		AppendComp<uint8_t>(Buf, 1);     // slot_idx = 1
		// component delta (sectionMask-framed)
		AppendComp<uint8_t>(Buf, 0x01);  // scalars
		AppendComp<uint8_t>(Buf, 1 << 0);  // ExtraHp bit
		AppendComp<int32_t>(Buf, 7);
		atlas::SpanReader R(Buf.data(), Buf.size());
		TestTrue(TEXT("component delta applies"), Entity.ApplyDelta(R));
	}
	atlas::mvp::StressLoadComponent* Load = Entity.load();
	if (!TestNotNull(TEXT("load slot bound"), Load)) { AtlasEdrDestroy(Ctx); return false; }
	TestEqual(TEXT("ExtraHp via typed getter"), Load->ExtraHp(), 7);

	// Upstream RPC: Charge(seq=42, amount=10) → cell RPC with slot=1 baked
	// into rpc_id bits 24-30.
	{
		Sender.Calls.clear();
		Load->Charge(42u, 10);
		if (!TestEqual(TEXT("1 cell rpc"), static_cast<int32>(Sender.Calls.size()), 1))
		{
			AtlasEdrDestroy(Ctx); return false;
		}
		const auto& C = Sender.Calls[0];
		TestFalse(TEXT("cell direction"), C.Base);
		const uint8_t slot = static_cast<uint8_t>((C.RpcId >> 24) & 0x7F);
		const uint8_t dir = static_cast<uint8_t>((C.RpcId >> 22) & 0x3);
		const uint8_t method = static_cast<uint8_t>(C.RpcId & 0xFF);
		TestEqual(TEXT("slot=1 in rpc_id"), slot, static_cast<uint8_t>(1));
		TestEqual(TEXT("dir=2 (cell)"), dir, static_cast<uint8_t>(2));
		TestEqual(TEXT("method=1 (Charge)"), method, static_cast<uint8_t>(1));
		// Payload = [u32 seq][int32 amount]
		TestEqual(TEXT("8-byte payload"), static_cast<int32>(C.Args.size()), 8);
		uint32_t seq = 0; int32_t amt = 0;
		std::memcpy(&seq, C.Args.data(), 4);
		std::memcpy(&amt, C.Args.data() + 4, 4);
		TestEqual(TEXT("seq=42"), seq, 42u);
		TestEqual(TEXT("amount=10"), amt, 10);
	}

	// Entity DispatchRpc strips slot=1 from rpc_id → routes to component's
	// DispatchRpc → kMethodIdx_OnAffixesUpdated = 1.
	{
		// rpc_id = (slot=1 << 24) | (dir=0 << 22) | (entity_type=3 << 8) | method=1
		const uint32_t RpcId = (1u << 24) | (0u << 22)
			| (static_cast<uint32_t>(atlas::mvp::StressAvatar::kTypeId) << 8)
			| 1u;
		std::vector<uint8_t> Buf;
		AppendComp<uint16_t>(Buf, 2);   // list count
		AppendComp<int32_t>(Buf, 3);
		AppendComp<int32_t>(Buf, 9);
		atlas::SpanReader R(Buf.data(), Buf.size());
		TestTrue(TEXT("dispatch routed to component"), Entity.DispatchRpc(RpcId, 0, R));
		TestEqual(TEXT("captured 1 affix call"),
			static_cast<FCaptureLoad*>(Load)->OnAffixesCount, 1);
		const auto& A = static_cast<FCaptureLoad*>(Load)->LastAffixes;
		if (TestEqual(TEXT("2 affixes"), static_cast<int32>(A.size()), 2))
		{
			TestEqual(TEXT("affix 0"), A[0], 3);
			TestEqual(TEXT("affix 1"), A[1], 9);
		}
	}

	AtlasEdrDestroy(Ctx);
	return true;
}

#endif  // WITH_DEV_AUTOMATION_TESTS
