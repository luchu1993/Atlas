#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include <cstring>
#include <string>
#include <variant>
#include <vector>

#include "AtlasCore/aoi_envelope.h"
#include "AtlasCore/client_entity.h"
#include "AtlasCore/client_entity_manager.h"
#include "AtlasCore/property_value.h"
#include "AtlasCore/span_reader.h"
#include "AtlasUE.h"
#include "entitydef/entitydef_api.h"

namespace
{
template <typename T>
void AppendApplyDelta(std::vector<uint8_t>& buf, T v)
{
	const auto offset = buf.size();
	buf.resize(offset + sizeof(T));
	std::memcpy(buf.data() + offset, &v, sizeof(T));
}

// Avatar.def declaration order is [hp, gold, aiState, modelId, mana, secret, level].
// Client-visible filter keeps [hp, modelId, mana, secret, level] — ReplicatedDirtyFlags
// bits 0..4 in that order, matching the C# Avatar.Properties.g.cs enum values.
constexpr uint16_t kAvatarTypeId = 2;
constexpr uint8_t kHpBit = 1 << 0;
constexpr uint8_t kManaBit = 1 << 2;
constexpr uint8_t kLevelBit = 1 << 4;
}  // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAtlasClientEntityApplyDeltaTest,
	"Atlas.ClientEntity.ApplyDelta",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAtlasClientEntityApplyDeltaTest::RunTest(const FString&)
{
	AtlasEdrContext* Ctx = FAtlasUEModule::GetEdrContext();
	if (!TestNotNull(TEXT("edr context loaded"), Ctx)) return false;

	const AtlasEdrEntity* AvatarDesc = AtlasEdrFindEntityById(Ctx, kAvatarTypeId);
	if (!TestNotNull(TEXT("avatar descriptor found"), AvatarDesc)) return false;
	const int32 PropCount = AtlasEdrEntityPropertyCount(AvatarDesc);
	TestEqual(TEXT("avatar descriptor prop count"), PropCount, 7);

	atlas::ClientEntity Entity(42, kAvatarTypeId);
	Entity.BindDescriptor(AvatarDesc, Ctx);
	TestEqual(TEXT("storage sized to descriptor"), static_cast<int32>(Entity.Properties().size()),
		PropCount);

	// sectionMask=0x01, scalarFlags = Hp | Mana | Level (skips ModelId, Secret).
	std::vector<uint8_t> Buf;
	AppendApplyDelta<uint8_t>(Buf, 0x01);
	AppendApplyDelta<uint8_t>(Buf, kHpBit | kManaBit | kLevelBit);
	AppendApplyDelta<int32_t>(Buf, 250);   // hp
	AppendApplyDelta<int32_t>(Buf, 75);    // mana
	AppendApplyDelta<int32_t>(Buf, 12);    // level

	atlas::SpanReader Reader(Buf.data(), Buf.size());
	TestTrue(TEXT("apply delta succeeds"), Entity.ApplyDelta(Reader));

	const auto& Props = Entity.Properties();
	// hp lives at descriptor index 0; non-visible props (gold, aiState) and
	// unmarked-dirty visible props (modelId, secret) stay as monostate.
	if (TestTrue(TEXT("hp slot is int32"), std::holds_alternative<int32_t>(Props[0])))
		TestEqual(TEXT("hp value"), std::get<int32_t>(Props[0]), 250);
	TestTrue(TEXT("gold untouched"), std::holds_alternative<std::monostate>(Props[1]));
	TestTrue(TEXT("aiState untouched"), std::holds_alternative<std::monostate>(Props[2]));
	TestTrue(TEXT("modelId untouched (not dirty)"),
		std::holds_alternative<std::monostate>(Props[3]));
	if (TestTrue(TEXT("mana slot is int32"), std::holds_alternative<int32_t>(Props[4])))
		TestEqual(TEXT("mana value"), std::get<int32_t>(Props[4]), 75);
	TestTrue(TEXT("secret untouched (not dirty)"),
		std::holds_alternative<std::monostate>(Props[5]));
	if (TestTrue(TEXT("level slot is int32"), std::holds_alternative<int32_t>(Props[6])))
		TestEqual(TEXT("level value"), std::get<int32_t>(Props[6]), 12);

	// Second delta carries a string (secret) — exercises PackedUInt32-prefixed UTF-8.
	{
		std::vector<uint8_t> Buf2;
		AppendApplyDelta<uint8_t>(Buf2, 0x01);
		AppendApplyDelta<uint8_t>(Buf2, 1 << 3);  // Secret bit
		const char* kSecret = "token-xyz";
		AppendApplyDelta<uint8_t>(Buf2, 9);  // PackedUInt32: len < 0xFE → single byte
		for (int i = 0; i < 9; ++i) AppendApplyDelta<uint8_t>(Buf2, static_cast<uint8_t>(kSecret[i]));

		atlas::SpanReader Reader2(Buf2.data(), Buf2.size());
		TestTrue(TEXT("apply delta string succeeds"), Entity.ApplyDelta(Reader2));
		if (TestTrue(TEXT("secret slot is string"),
				std::holds_alternative<std::string>(Entity.Properties()[5])))
			TestEqual(TEXT("secret value"), FString(std::get<std::string>(Entity.Properties()[5]).c_str()),
				FString("token-xyz"));
	}

	// Truncated body — reader runs out mid-scalar.
	{
		std::vector<uint8_t> Buf3;
		AppendApplyDelta<uint8_t>(Buf3, 0x01);
		AppendApplyDelta<uint8_t>(Buf3, kHpBit);
		AppendApplyDelta<uint8_t>(Buf3, 0);  // only 1 of 4 hp bytes
		atlas::SpanReader Reader3(Buf3.data(), Buf3.size());
		TestFalse(TEXT("truncated delta fails"), Entity.ApplyDelta(Reader3));
	}

	// End-to-end: ClientEntityManager with edr_ctx auto-binds on HandleEnter,
	// then a kEntityPropertyUpdate envelope flows through DecodeAoIEnvelope
	// into the same generic ApplyDelta path.
	{
		atlas::ClientEntityManager Manager;
		Manager.SetDescriptorContext(Ctx);
		Manager.RegisterFactory(kAvatarTypeId, [](atlas::EntityId Id, atlas::EntityTypeId T) {
			return std::make_unique<atlas::ClientEntity>(Id, T);
		});
		const atlas::Vec3 Zero{};
		TestTrue(TEXT("HandleEnter creates avatar"),
			Manager.HandleEnter(123, kAvatarTypeId, 0.0, Zero, Zero, true));
		auto* E = Manager.Find(123);
		if (!TestNotNull(TEXT("avatar bound to descriptor"),
				E ? E->Descriptor() : nullptr)) return false;

		// kEntityPropertyUpdate: [u8 kind][u32 id][u64 event_seq][sectionMask][flags][values...]
		std::vector<uint8_t> Envelope;
		AppendApplyDelta<uint8_t>(Envelope, static_cast<uint8_t>(atlas::EnvelopeKind::kEntityPropertyUpdate));
		AppendApplyDelta<uint32_t>(Envelope, 123);
		AppendApplyDelta<uint64_t>(Envelope, 1);  // event_seq
		AppendApplyDelta<uint8_t>(Envelope, 0x01);  // sectionMask scalars
		AppendApplyDelta<uint8_t>(Envelope, kHpBit | kLevelBit);
		AppendApplyDelta<int32_t>(Envelope, 999);  // hp
		AppendApplyDelta<int32_t>(Envelope, 42);   // level

		TestEqual(TEXT("envelope decode ok"),
			atlas::DecodeAoIEnvelope(Envelope.data(), Envelope.size(), Manager),
			atlas::EnvelopeDecodeResult::kOk);
		const auto& E2eProps = E->Properties();
		if (TestTrue(TEXT("e2e hp int32"), std::holds_alternative<int32_t>(E2eProps[0])))
			TestEqual(TEXT("e2e hp value"), std::get<int32_t>(E2eProps[0]), 999);
		if (TestTrue(TEXT("e2e level int32"), std::holds_alternative<int32_t>(E2eProps[6])))
			TestEqual(TEXT("e2e level value"), std::get<int32_t>(E2eProps[6]), 42);
	}

	return true;
}

#endif  // WITH_DEV_AUTOMATION_TESTS
